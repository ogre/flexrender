#include "engine.hpp"

#include <cassert>
#include <sstream>
#include <limits>
#include <vector>
#include <utility>
#include <ctime>
#include <semaphore.h>
#include <stdio.h>
#include <errno.h>

#include "uv.h"

#include "scripting.hpp"
#include "types.hpp"
#include "utils.hpp"

/// How long to wait for more data before flushing the send buffer.
#define FR_FLUSH_TIMEOUT_MS 10

using std::string;
using std::stringstream;
using std::numeric_limits;
using std::vector;
using std::pair;
using std::make_pair;

namespace fr {

/// The library for the current render.
static Library* lib = nullptr;

/// The number of workers that we've connected to.
static size_t num_workers_connected = 0;

/// The number of workers that are syncing.
static size_t num_workers_syncing = 0;

/// The number of workers that have finished building their local BVHs.
static size_t num_workers_built = 0;

/// The number of workers that are ready to render.
static size_t num_workers_ready = 0;

/// The number of workers that have sent and merged their images.
static size_t num_workers_complete = 0;

/// The maximum number of uninteresting stats intervals before we declare the
/// rendering complete.
static uint32_t max_intervals = 0;

/// Whether or not to use a worker BVH or a simple linear scan for network
/// traversal.
static bool use_linear_scan = false;

/// The timer for ensuring we flush the send buffer.
static uv_timer_t flush_timer;

/// Timer for watching whether or not the render is still interesting.
static uv_timer_t interesting_timer;

/// Timer for checking runaway conditions.
static uv_timer_t runaway_timer;

/// Synchronization primitives for synchronizing the synchronization.
/// "We need to go deeper..."
static sem_t mesh_read;
static sem_t mesh_synced;

/// The ID of the mesh we're currently syncing over the network.
static uint32_t current_mesh_id = 0;

/// The scene file we're rendering.
static string scene;

/// The bounding boxes of all participating workers.
static vector<pair<uint32_t, BoundingBox>> worker_bounds;

/// Timers for measuring total time.
static time_t sync_start;
static time_t sync_stop;
static time_t build_start;
static time_t build_stop;
static time_t render_start;
static time_t render_stop;

// Callbacks, handlers, and helpers for client functionality.
namespace client {

void Init();
void DispatchMessage(NetNode* node);
void StartSync();
uint32_t SyncMesh(Mesh* mesh);
void BuildWBVH();
void StartRender();
void StopRender();

void OnConnect(uv_connect_t* req, int status);
uv_buf_t OnAlloc(uv_handle_t* handle, size_t suggested_size);
void OnRead(uv_stream_t* stream, ssize_t nread, uv_buf_t buf);
void OnClose(uv_handle_t* handle);
void OnFlushTimeout(uv_timer_t* timer, int status);
void OnInterestingTimeout(uv_timer_t* timer, int status);
void OnRunawayTimeout(uv_timer_t* timer, int status);
void OnSyncStart(uv_work_t* req);
void AfterSync(uv_work_t* req, int status);
void OnSyncIdle(uv_idle_t* handle, int status);

void OnOK(NetNode* node);
void OnSyncImage(NetNode* node);
void OnRenderStats(NetNode* node);

}

void EngineInit(const string& config_file, const string& scene_file,
 uint32_t intervals, bool linear_scan) {
    lib = new Library;

    max_intervals = intervals;
    use_linear_scan = linear_scan;

    // Parse the config file.
    ConfigScript config_script;
    TOUTLN("Loading config from " << config_file << ".");
    if (!config_script.Parse(config_file, lib)) {
        TERRLN("Can't continue with bad config.");
        exit(EXIT_FAILURE);
    }
    TOUTLN("Config loaded.");

    scene = scene_file;

    client::Init();
}

void EngineRun() {
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
}

void client::Init() {
    int result = 0;
    struct sockaddr_in addr;

    Config* config = lib->LookupConfig();

    TOUTLN("Connecting to " << config->workers.size() << " workers...");

    for (size_t i = 0; i < config->workers.size(); i++) {
        const auto& worker = config->workers[i];

        NetNode* node = new NetNode(DispatchMessage, worker);

        // Initialize the TCP socket.
        result = uv_tcp_init(uv_default_loop(), &node->socket);
        CheckUVResult(result, "tcp_init");
        
        // Squirrel away the node in the data baton.
        node->socket.data = node;

        // Connect to the server.
        addr = uv_ip4_addr(node->ip.c_str(), node->port);
        uv_connect_t* req = reinterpret_cast<uv_connect_t*>(malloc(sizeof(uv_connect_t)));
        result = uv_tcp_connect(req, &node->socket, addr, OnConnect);
        CheckUVResult(result, "tcp_connect");

        // Add the node to the library.
        lib->StoreNetNode(i + 1, node); // +1 because 0 is reserved
    }

    // Initialize the flush timeout timer.
    result = uv_timer_init(uv_default_loop(), &flush_timer);
    CheckUVResult(result, "timer_init");
    result = uv_timer_start(&flush_timer, OnFlushTimeout, FR_FLUSH_TIMEOUT_MS,
     FR_FLUSH_TIMEOUT_MS);
    CheckUVResult(result, "timer_start");
    
    // Initialize the interesting timer.
    result = uv_timer_init(uv_default_loop(), &interesting_timer);
    CheckUVResult(result, "timer_init");

    // Initialize the runaway timer.
    result = uv_timer_init(uv_default_loop(), &runaway_timer);
    CheckUVResult(result, "timer_init");
}

void client::DispatchMessage(NetNode* node) {
    switch (node->message.kind) {
        case Message::Kind::OK:
            OnOK(node);
            break;

        case Message::Kind::RENDER_STATS:
            OnRenderStats(node);
            break;

        case Message::Kind::SYNC_IMAGE:
            OnSyncImage(node);
            break;

        default:
            TERRLN("Received unexpected message.");
            TERRLN(ToString(node->message));
            break;
    }
}

void client::OnConnect(uv_connect_t* req, int status) {
    assert(req != nullptr);
    assert(req->handle != nullptr);
    assert(req->handle->data != nullptr);

    int result = 0;

    // Pull the net node out of the data baton.
    NetNode* node = reinterpret_cast<NetNode*>(req->handle->data);
    free(req);

    if (status != 0) {
        TERRLN("Failed connecting to " << node->ip << ".");
        exit(EXIT_FAILURE);
    }

    TOUTLN("[" << node->ip << "] Connected on port " << node->port << ".");

    // Start reading replies from the server.
    result = uv_read_start(reinterpret_cast<uv_stream_t*>(&node->socket),
     OnAlloc, OnRead);
    CheckUVResult(result, "read_start");

    // Nothing else to do if we're still waiting for everyone to connect.
    num_workers_connected++;
    if (num_workers_connected < lib->LookupConfig()->workers.size()) {
        return;
    }

    sync_start = time(nullptr);

    // Send init messages to each server.
    lib->ForEachNetNode([](uint32_t id, NetNode* node) {
        Message request(Message::Kind::INIT);
        request.size = sizeof(uint32_t);
        request.body = &id;
        node->state = NetNode::State::INITIALIZING;
        node->Send(request);

        node->me = id;
    });
}

uv_buf_t client::OnAlloc(uv_handle_t* handle, size_t suggested_size) {
    assert(handle != nullptr);
    assert(handle->data != nullptr);

    // Just allocate a buffer of the suggested size.
    uv_buf_t buf;
    buf.base = reinterpret_cast<char*>(malloc(suggested_size));
    buf.len = suggested_size;

    return buf;
}

void client::OnRead(uv_stream_t* stream, ssize_t nread, uv_buf_t buf) {
    assert(stream != nullptr);
    assert(stream->data != nullptr);

    // Pull the net node out of the data baton.
    NetNode* node = reinterpret_cast<NetNode*>(stream->data);

    if (nread < 0) {
        // No data was read.
        uv_err_t err = uv_last_error(uv_default_loop());
        if (err.code == UV_EOF) {
            uv_close(reinterpret_cast<uv_handle_t*>(&node->socket), OnClose);
        } else {
            TERRLN("read: " << uv_strerror(err));
        }
    } else if (nread > 0) {
        // Data is available, parse any new messages out.
        node->Receive(buf.base, nread);
    }

    if (buf.base) {
        free(buf.base);
    }
}

void client::OnClose(uv_handle_t* handle) {
    assert(handle != nullptr);
    assert(handle->data != nullptr);

    // Pull the net node out of the data baton.
    NetNode* node = reinterpret_cast<NetNode*>(handle->data);

    TOUTLN("[" << node->ip << "] Disconnected.");

    // Net node will be deleted with library.
}

void client::OnFlushTimeout(uv_timer_t* timer, int status) {
    assert(timer == &flush_timer);
    assert(status == 0);

    lib->ForEachNetNode([](uint32_t id, NetNode* node) {
        if (!node->flushed && node->nwritten > 0) {
            node->Flush();
        }
        node->flushed = false;
    });
}

void client::OnInterestingTimeout(uv_timer_t* timer, int status) {
    assert(timer == &interesting_timer);
    assert(status == 0);

    // If all net nodes are no longer interesting, stop the render.
    bool done = true;
    lib->ForEachNetNode([&done](uint32_t id, NetNode* node) {
        done = done && !node->IsInteresting(max_intervals);
    });

    // Are we done rendering?
    if (done) {
        TOUTLN("Workers are no longer interesting.");
        StopRender();
        return;
    }

    // Display some information about the total number of rays being processed.
    uint64_t total_produced = 0;
    uint64_t total_killed = 0;
    uint64_t total_queued = 0;
    lib->ForEachNetNode([&total_produced, &total_killed, &total_queued](uint32_t id, NetNode* node) {
        total_produced += node->RaysProduced(max_intervals);
        total_killed += node->RaysKilled(max_intervals);
        total_queued += node->RaysQueued(max_intervals);
    });
    TOUTLN("RAYS:  +" << total_produced << "  -" << total_killed << "  ~" << total_queued);
}

void client::OnRunawayTimeout(uv_timer_t* timer, int status) {
    assert(timer == &runaway_timer);
    assert(status == 0);

    // How far along is the slowest worker?
    float slowest = numeric_limits<float>::infinity();
    lib->ForEachNetNode([&slowest](uint32_t id, NetNode* node) {
        float progress = node->Progress();
        if (progress < slowest) {
            slowest = progress;
        }
    });

    Config* config = lib->LookupConfig();
    float runaway = config->runaway;

    // Pause each worker that is more than runaway ahead of the slowest.
    lib->ForEachNetNode([slowest, runaway](uint32_t id, NetNode* node) {
        float progress = node->Progress();
        if (progress > slowest + runaway) {
            if (node->state != NetNode::State::PAUSED) {
                TOUTLN("[" << node->ip << "] Runaway detected. Pausing work generation.");
                Message request(Message::Kind::RENDER_PAUSE);
                node->state = NetNode::State::PAUSED;
                node->Send(request);
            }
        } else if (progress <= slowest) {
            if (node->state == NetNode::State::PAUSED) {
                TOUTLN("[" << node->ip << "] Runaway eliminated. Resuming work generation.");
                Message request(Message::Kind::RENDER_RESUME);
                node->state = NetNode::State::RENDERING;
                node->Send(request);
            }
        }
    });
}

void client::OnOK(NetNode* node) {
    Config* config = lib->LookupConfig();
    assert(config != nullptr);

    switch (node->state) {
        case NetNode::State::INITIALIZING:
            node->state = NetNode::State::CONFIGURING;
            TOUTLN("[" << node->ip << "] Configuring worker.");
            node->SendConfig(lib);
            break;

        case NetNode::State::CONFIGURING:
            node->state = NetNode::State::SYNCING_ASSETS;
            TOUTLN("[" << node->ip << "] Ready to sync.");
            num_workers_syncing++;
            if (num_workers_syncing == config->workers.size()) {
                StartSync();
            }
            break;

        case NetNode::State::SYNCING_ASSETS:
            // Delete the current mesh from the library.
            lib->StoreMesh(current_mesh_id, nullptr);
            if (sem_post(&mesh_synced) < 0) {
                perror("sem_post");
                exit(EXIT_FAILURE);
            }
            break;

        case NetNode::State::SYNCING_CAMERA:
            node->state = NetNode::State::SYNCING_EMISSIVE;
            TOUTLN("[" << node->ip << "] Syncing list of emissive workers.");
            node->SendLightList(lib);
            break;

        case NetNode::State::SYNCING_EMISSIVE:
            {
                Message request(Message::Kind::BUILD_BVH);
                node->state = NetNode::State::BUILDING_BVH;
                node->Send(request);
                TOUTLN("[" << node->ip << "] Building local BVH.");
            }
            break;

        case NetNode::State::BUILDING_BVH:
            {
                assert(node->message.size == sizeof(BoundingBox));
                BoundingBox bounds = *(reinterpret_cast<BoundingBox*>(node->message.body));
                worker_bounds.emplace_back(make_pair(node->me, bounds));

                TOUTLN("[" << node->ip << "] Local BVH ready.");
                num_workers_built++;

                if (use_linear_scan) {
                    // Jump right into starting the render.
                    node->state = NetNode::State::SYNCING_WBVH;
                    OnOK(node);
                } else {
                    // Build the worker BVH and distribute it if all workers
                    // have reported in.
                    if (num_workers_built == config->workers.size()) {
                        BuildWBVH();
                    }
                }
            }
            break;

        case NetNode::State::SYNCING_WBVH:
            node->state = NetNode::State::READY;
            TOUTLN("[" << node->ip << "] Ready to render.");
            num_workers_ready++;
            if (num_workers_ready == config->workers.size()) {
                StartRender();
            }
            break;

        default:
            TERRLN("Received OK in unexpected state.");
    }
}

void client::OnRenderStats(NetNode* node) {
    assert(node != nullptr);
    node->ReceiveRenderStats();
}

void client::OnSyncImage(NetNode* node) {
    assert(node != nullptr);
    assert(lib != nullptr);

    int result = 0;

    Config* config = lib->LookupConfig();
    assert(config != nullptr);

    Image* final = lib->LookupImage();
    assert(final != nullptr);

    Image* component = node->ReceiveImage();
    assert(component != nullptr);

    // Create the component filename.
    stringstream component_file;
    component_file << config->name << "-" << node->ip << "_" << node->port;

    // Write the component image out as name-worker.exr.
    TOUTLN("Writing image to " << component_file.str() << ".exr...");
    component->ToEXRFile(component_file.str() + ".exr");

    // Merge the component image with the final image.
    final->Merge(component);
    TOUTLN("[" << node->ip << "] Merged image.");

    // Done with the component image.
    delete component;

    // Write the render stats out as name-worker.csv.
    TOUTLN("Writing stats to " << component_file.str() << ".csv...");
    node->StatsToCSVFile(component_file.str() + ".csv");

    // Done for now if this wasn't the last worker.
    num_workers_complete++;
    if (num_workers_complete < config->workers.size()) {
        return;
    }

    // Write out the final image.
    final->ToEXRFile(config->name + ".exr");
    TOUTLN("Wrote " << config->name << ".exr.");

    // Dump out timers.
    TOUTLN("Time spent syncing: " << (sync_stop - sync_start) << " seconds.");
    if (!use_linear_scan) {
        TOUTLN("Time spent building: " << (build_stop - build_start) << " seconds.");
    }
    TOUTLN("Time spent rendering: " << (render_stop - render_start) << " seconds.");

    // Disconnect from each worker.
    lib->ForEachNetNode([config](uint32_t id, NetNode* node) {
        uv_close(reinterpret_cast<uv_handle_t*>(&node->socket), OnClose);
    });

    // Shutdown the flush timer.
    result = uv_timer_stop(&flush_timer);
    CheckUVResult(result, "timer_stop");
    uv_close(reinterpret_cast<uv_handle_t*>(&flush_timer), nullptr);
}

void client::StartSync() {
    int result = 0;

    // Build the spatial index.
    lib->BuildSpatialIndex();

    Config* config = lib->LookupConfig();
    assert(config != nullptr);

    // Create the image with all the requested buffers.
    Image* image = new Image(config->width, config->height);
    for (const auto& buffer : config->buffers) {
        image->AddBuffer(buffer);
    }
    lib->StoreImage(image);

    // Initialize the semaphores for ping-ponging between threads.
    if (sem_init(&mesh_read, 0, 0) < 0) {
        perror("sem_init");
        exit(EXIT_FAILURE);
    }
    if (sem_init(&mesh_synced, 0, 1) < 0) {
        perror("sem_init");
        exit(EXIT_FAILURE);
    }

    // Queue up the scene parsing to happen on the thread pool.
    uv_work_t* req = reinterpret_cast<uv_work_t*>(malloc(sizeof(uv_work_t)));
    result = uv_queue_work(uv_default_loop(), req, OnSyncStart, AfterSync);
    CheckUVResult(result, "queue_work");

    // Start up the idle callback for handling networking.
    uv_idle_t* idler = reinterpret_cast<uv_idle_t*>(malloc(sizeof(uv_idle_t)));
    result = uv_idle_init(uv_default_loop(), idler);
    CheckUVResult(result, "idle_init");
    result = uv_idle_start(idler, OnSyncIdle);
    CheckUVResult(result, "idle_start");
}

void client::BuildWBVH() {
    TOUTLN("Building WBVH.");

    // Build the worker BVH from the worker extents.
    BVH* wbvh = new BVH(worker_bounds);
    TOUTLN("Worker BVH size: " << wbvh->GetSizeInBytes() << " bytes");

    lib->ForEachNetNode([wbvh](uint32_t id, NetNode* node) {
        node->state = NetNode::State::SYNCING_WBVH;
        TOUTLN("[" << node->ip << "] Syncing WBVH.");
        node->SendWBVH(wbvh);
    });

    build_stop = time(nullptr);

    // We don't need it anymore.
    delete wbvh;
}

void client::StartRender() {
    int result = 0;

    Config* config = lib->LookupConfig();

    sync_stop = time(nullptr);
    render_start = time(nullptr);

    // Send render start messages to each server.
    lib->ForEachNetNode([config](uint32_t id, NetNode* node) {
        uint16_t chunk_size = config->width / config->workers.size();
        int16_t offset = (id - 1) * chunk_size;
        if (id == config->workers.size()) {
            chunk_size = config->width - (id - 1) * chunk_size;
        }
        uint32_t payload = (offset << 16) | chunk_size;

        Message request(Message::Kind::RENDER_START);
        request.size = sizeof(uint32_t);
        request.body = &payload;
        node->Send(request);

        node->state = NetNode::State::RENDERING;
        TOUTLN("[" << node->ip << "] Starting render.");
    });

    // Start the interesting timer.
    result = uv_timer_start(&interesting_timer, OnInterestingTimeout,
     FR_STATS_TIMEOUT_MS * max_intervals, FR_STATS_TIMEOUT_MS * max_intervals);
    CheckUVResult(result, "timer_start");

    // Start the runaway timer.
    result = uv_timer_start(&runaway_timer, OnRunawayTimeout,
     FR_STATS_TIMEOUT_MS, FR_STATS_TIMEOUT_MS);
    CheckUVResult(result, "timer_start");

    TOUTLN("Rendering has started.");
}

void client::StopRender() {
    int result = 0;

    render_stop = time(nullptr);

    // Stop the interesting timer.
    result = uv_timer_stop(&interesting_timer);
    CheckUVResult(result, "timer_stop");
    uv_close(reinterpret_cast<uv_handle_t*>(&interesting_timer), nullptr);

    // Stop the runaway_timer.
    result = uv_timer_stop(&runaway_timer);
    CheckUVResult(result, "timer_stop");
    uv_close(reinterpret_cast<uv_handle_t*>(&runaway_timer), nullptr);

    // Send render stop messages to each server.
    lib->ForEachNetNode([](uint32_t id, NetNode* node) {
        Message request(Message::Kind::RENDER_STOP);
        node->Send(request);
        node->state = NetNode::State::SYNCING_IMAGES;
        TOUTLN("[" << node->ip << "] Stopping render.");
    });

    TOUTLN("Rendering has stopped, syncing images.");
}

void client::OnSyncStart(uv_work_t* req) {
    // !!! WARNING !!!
    // Everything this function does and calls must be thread-safe. This
    // function will NOT run in the main thread, it runs on the thread pool.

    assert(req != nullptr);

    // Parse and distribute the scene.
    SceneScript scene_script(SyncMesh);
    TOUTLN("Loading scene from " << scene << ".");
    if (!scene_script.Parse(scene, lib)) {
        TERRLN("Can't continue with bad scene.");
        exit(EXIT_FAILURE);
    }

    // Signal that we're finished.
    SyncMesh(nullptr);
    TOUTLN("Scene distributed.");
}

void client::AfterSync(uv_work_t* req, int status) {
    assert(req != nullptr);
    free(req);
}

uint32_t client::SyncMesh(Mesh* mesh) {
    // !!! WARNING !!!
    // Everything this function does and calls must be thread-safe. This
    // function will NOT run in the main thread, it runs on the thread pool.

    // Wait for the main thread to be finished with the network.
    if (sem_wait(&mesh_synced) < 0) {
        perror("sem_wait");
        exit(EXIT_FAILURE);
    }

    uint32_t id = 0;
    if (mesh != nullptr) {
        // Store the mesh in the library and get back its ID.
        id = lib->NextMeshID();
        mesh->id = id;
        lib->StoreMesh(id, mesh);
    }

    // Tell the main thread which mesh we'd like to sync over the network.
    current_mesh_id = id;

    // Wake up the main thread to do the networking.
    if (sem_post(&mesh_read) < 0) {
        perror("sem_post");
        exit(EXIT_FAILURE);
    }

    return id;
}

void client::OnSyncIdle(uv_idle_t* handle, int status) {
    assert(handle != nullptr);
    assert(status == 0);

    int result = 0;

    // Are we done reading in a new mesh?
    if (sem_trywait(&mesh_read) < 0) {
        if (errno == EAGAIN) {
            // Nope.
            return;
        }
        perror("sem_trywait");
        exit(EXIT_FAILURE);
    }

    // Are we done syncing assets?
    if (current_mesh_id == 0) {
        // Shut off the idle handler.
        result = uv_idle_stop(handle);
        CheckUVResult(result, "idle_stop");
        uv_close(reinterpret_cast<uv_handle_t*>(handle), nullptr);

        // Sync the camera with everyone.
        lib->ForEachNetNode([](uint32_t id, NetNode* node) {
            node->state = NetNode::State::SYNCING_CAMERA;
            TOUTLN("[" << node->ip << "] Syncing camera.");
            node->SendCamera(lib);
        });

        build_start = time(nullptr);

        return;
    }

    Mesh* mesh = lib->LookupMesh(current_mesh_id);
    assert(mesh != nullptr);
    Config* config = lib->LookupConfig();
    assert(config != nullptr);

    uint64_t spacecode = SpaceEncode(mesh->centroid, config->min, config->max);
    uint32_t id = lib->LookupNetNodeBySpaceCode(spacecode);
    NetNode* node = lib->LookupNetNode(id);

    TOUTLN("[" << node->ip << "] Sending mesh " << current_mesh_id << " to worker " << id << ".");

    node->SendMesh(lib, current_mesh_id);
}

} // namespace fr
