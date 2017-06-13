/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "guest/gce_network/metadata_proxy.h"

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <list>

#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <glog/logging.h>

#include "common/libs/auto_resources/auto_resources.h"
#include "common/libs/fs/shared_fd.h"
#include "common/libs/fs/shared_select.h"


namespace avd {
namespace {

const char kMetadataString[] =
    "{"
    "   \"project\": {"
    "       \"attributes\": {"
    "           \"virtual_device_zone\": \"us-central2-b\""
    "       }"
    "   },"
    "   \"instance\": {"
    "       \"hostname\": \"aosp_cf_x86.internal\","
    "       \"attributes\": {"
    "           \"cfg_sta_display_resolution\": \"800x1280x16x213\","
    "           \"cfg_sta_persistent_data_device\": \"default\","
    "           \"cfg_sta_ephemeral_cache_size_mb\": \"512\","
    "           \"cfg_sta_initial_locale\": \"en_US\","
    "           \"camera_front\": \"1,768,1024,checker-sliding\","
    "           \"camera_back\": \"1,768,1024,checker-fixed\","
    "           \"gps_coordinates\": \"37.421577,-122.083579,0,0,0,10\","
    "           \"power_battery_capacity\": \"90\""
    "       }"
    "   }"
    "}";

bool SaveInitialMetadata(const AutoFreeBuffer& content) {
  AutoCloseFileDescriptor fd(
      open("/initial.metadata",
           O_CREAT|O_TRUNC|O_WRONLY, 0600));
  char headers[] = "\r\n";
  size_t headers_size = strlen(headers);

  if (fd.IsError()) {
    LOG(ERROR) << "Failed to create initial metadata file ("
               << strerror(errno) << ").";
    return false;
  }

  if (TEMP_FAILURE_RETRY(write(fd, headers, headers_size))
      != static_cast<int>(headers_size)) {
    LOG(ERROR) << "Failed to write " << headers_size << " bytes to initial metadata file: "
              << errno << "(" << strerror(errno) << ").";
    return false;
  }

  if (TEMP_FAILURE_RETRY(write(fd, content.data(), content.size()))
      != static_cast<int>(content.size())) {
    LOG(ERROR) << "Failed to write " << content.size() << " bytes to initial metadata file: "
               << errno << "(" << strerror(errno) << ").";
    return false;
  }

  LOG(INFO) << "Successfully stored " << content.size() << " bytes in initial.metadata file.";

  fd.close();
  // So that any HAL instances can read the initial config.
  chmod("/initial.metadata", 0644);
  return true;
}

// Implementation of MetadataProxy interface.
// Starts a background threads polling the GCE Metadata Server for updates.
// Notifies all connected clients about metadata updates.
class MetadataProxyImpl : public MetadataProxy {
 public:
  MetadataProxyImpl(
      SysClient* client, NetworkNamespaceManager* ns_manager)
      : client_(client),
        ns_manager_(ns_manager) {
    initial_metadata_.PrintF(kMetadataString);
    metadata_.PrintF(kMetadataString);
  }

  ~MetadataProxyImpl() {}

  // Send metadata update to specified clients.
  //
  // Returns true, if sending metadata was successful.
  bool SendMetadata(SharedFD client, AutoFreeBuffer& metadata) {
    int32_t length = metadata.size();
    // Do we have anything to send?
    // If not, we probably failed to fetch update from metadata server.
    if (length == 0) return true;
    if ((client->Send(&length, sizeof(length), MSG_NOSIGNAL) < 0) ||
        (client->Send(metadata.data(), length, MSG_NOSIGNAL) < 0)) {
      LOG(WARNING) << "Dropping metadata client: write error "
                   << strerror(errno);
      return false;
    }
    return true;
  }


  int StartProxy(const std::string& socket_name) {
    SharedFD server_sock = CreateServerSocket(socket_name);

    LOG(INFO) << "Starting metadata proxy service. Listening on @"
              << socket_name;

    while(true) {
      // Wait for signal from either server socket (that new client has
      // connected), or any of the client sockets, indicating clients have
      // closed their end.
      SharedFDSet wait_set;
      wait_set.Set(server_sock);
      for (std::list<SharedFD>::iterator client = clients_.begin();
           client != clients_.end(); ++client) {
        wait_set.Set(*client);
      }
      Select(&wait_set, NULL, NULL, NULL);

      // Check if new client connected.
      if (wait_set.IsSet(server_sock)) {
        AcceptNewClient(server_sock);
      }

      // Detect existing client disconnected.
      for (std::list<SharedFD>::iterator client = clients_.begin();
           client != clients_.end();) {
        if (wait_set.IsSet(*client)) {
          LOG(INFO) << "Metadata proxy client disconnected.";
          client = clients_.erase(client);
        } else {
          ++client;
        }
      }
    }
    // Not reached, but we need a return value for some compilers.
    return 0;
  }

  // Metadata proxy process body.
  // Accepts new clients of metadata proxy socket named |socket_name|.
  // Upon connection sends two complete metadata updates:
  // - initial metadata (first update ever),
  // - current metadata (may be same as initial metadata).
  bool Start(const std::string& socket_name) {
    SaveInitialMetadata(initial_metadata_);
    // getpid() and gettid() return 1 on this thread, so fork to get the process
    // into a saner state. Use this thread to monitor the child and restart it.
    while (true) {
      SysClient::ProcessHandle* h = client_->Clone(
          "gce.meta.proxy",
          [this, socket_name]() -> int32_t {
            StartProxy(socket_name);
            return 0;
          }, 0);
      h->WaitResult();

      // Wait a bit so we done flood with forks
      sleep(5);
    }
    return false;
  }

  // Create new server socket in an Android network namespace.
  //
  // Unix domain sockets are associated with network namespaces.
  // This means that an unix socket created in namespace A will not be
  // accessible from namespace B.
  //
  // Now that the metadata client thread has started, reparent this thread, so
  // that the unix socket is created in android namespace.
  SharedFD CreateServerSocket(const std::string& socket_name) {
    if (client_->SetNs(
        ns_manager_->GetNamespaceDescriptor(
            NetworkNamespaceManager::kAndroidNs), kCloneNewNet) < 0) {
      LOG(ERROR) << "Failed to switch namespace: " << strerror(errno);
      return SharedFD();
    }

    // Start listening for metadata updates.
    SharedFD server_sock = SharedFD::SocketLocalServer(
        socket_name.c_str(), true, SOCK_STREAM, 0666);

    if (!server_sock->IsOpen()) {
      LOG(ERROR) << "Failed to start local server "
                 << socket_name << ": " << strerror(errno);
      return SharedFD();
    }

    // Return to original network namespace.
    client_->SetNs(
        ns_manager_->GetNamespaceDescriptor(
            NetworkNamespaceManager::kOuterNs), kCloneNewNet);

    return server_sock;
  }

  // Accept new client connection on supplied server socket.
  void AcceptNewClient(SharedFD server_sock) {
    SharedFD client_sock = SharedFD::Accept(*server_sock);

    if (!client_sock->IsOpen()) {
      LOG(WARNING) << "Metadata proxy failed to connect new client.";
      return;
    }

    LOG(INFO) << "Accepted new metadata proxy client.";

    // Append client to all clients, if we could successfully send initial
    // update.
    if (SendMetadata(client_sock, initial_metadata_) &&
        SendMetadata(client_sock, metadata_)) {
      clients_.push_back(client_sock);
    }
  }

 private:
  SysClient* const client_;
  NetworkNamespaceManager* const ns_manager_;

  AutoFreeBuffer initial_metadata_;
  AutoFreeBuffer metadata_;
  std::list<SharedFD> clients_;

  MetadataProxyImpl(const MetadataProxyImpl&);
  MetadataProxyImpl& operator= (const MetadataProxyImpl&);
};

}  // namespace

MetadataProxy* MetadataProxy::New(
    SysClient* client,
    NetworkNamespaceManager* ns_manager) {
  return new MetadataProxyImpl(client, ns_manager);
}

}  // namespace avd
