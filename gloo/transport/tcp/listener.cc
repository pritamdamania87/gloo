/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gloo/transport/tcp/listener.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <unistd.h>

#include <iostream>

#include <gloo/common/common.h>
#include <gloo/common/logging.h>
#include <gloo/transport/tcp/helpers.h>

namespace gloo {
namespace transport {
namespace tcp {

Listener::Listener(std::shared_ptr<Loop> loop, const attr& attr)
    : loop_(std::move(loop)) {
  listener_ = Socket::createForFamily(attr.ai_addr.ss_family);
  listener_->reuseAddr(true);
  listener_->bind(attr.ai_addr);
  listener_->listen(kBacklog);
  addr_ = listener_->sockName();

  // Register with loop for readability events.
  loop_->registerDescriptor(listener_->fd(), EPOLLIN, this);
}

Listener::~Listener() {
  if (listener_) {
    loop_->unregisterDescriptor(listener_->fd());
  }
}

void Listener::handleEvents(int events) {
  std::lock_guard<std::mutex> guard(mutex_);

  for (;;) {
    auto sock = listener_->accept();
    if (!sock) {
      // Let the loop try again on the next tick.
      if (errno == EAGAIN) {
        return;
      }
      // Actual error.
      GLOO_ENFORCE(false, "accept: ", strerror(errno));
    }

    sock->reuseAddr(true);
    sock->noDelay(true);

    // Read sequence number.
    read<sequence_number_t>(
        loop_,
        sock,
        [this](
            std::shared_ptr<Socket> socket,
            const Error& error,
            sequence_number_t&& seq) {
          if (error) {
            std::cout << "got error!!!" << error.what() << std::endl;
            return;
          }

          haveConnection(std::move(socket), seq);
        });
  }
}

Address Listener::nextAddress() {
  std::lock_guard<std::mutex> guard(mutex_);
  return Address(addr_.getSockaddr(), seq_++);
}

void Listener::waitForConnection(
    const Address& addr,
    std::chrono::milliseconds timeout,
    connect_callback_t fn) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto seq = addr.getSeq();

  // If we don't yet have an fd for this sequence number, persist callback.
  auto it = seqToSocket_.find(seq);
  if (it == seqToSocket_.end()) {
    seqToCallback_.emplace(seq, std::move(fn));
    return;
  }

  // If we already have an fd for this sequence number, schedule callback.
  auto socket = std::move(it->second);
  seqToSocket_.erase(it);
  loop_->defer([fn, socket]() { fn(socket, Error::OK); });
}

void Listener::haveConnection(
    std::shared_ptr<Socket> socket,
    sequence_number_t seq) {
  std::unique_lock<std::mutex> lock(mutex_);

  // If we don't yet have a callback for this sequence number, persist socket.
  auto it = seqToCallback_.find(seq);
  if (it == seqToCallback_.end()) {
    seqToSocket_.emplace(seq, std::move(socket));
    return;
  }

  // If we already have a callback for this sequence number, trigger it.
  auto fn = std::move(it->second);
  seqToCallback_.erase(it);
  lock.unlock();
  fn(std::move(socket), Error::OK);
}

} // namespace tcp
} // namespace transport
} // namespace gloo
