/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>

#include <gloo/transport/tcp/error.h>
#include <gloo/transport/tcp/loop.h>
#include <gloo/transport/tcp/socket.h>

namespace gloo {
namespace transport {
namespace tcp {

template <typename T>
class BaseOperation_ : public std::enable_shared_from_this<T> {
 public:
  void initialize() {
    // Cannot initialize leak until after the object has been
    // constructed, because the std::make_shared initialization
    // doesn't run after construction of the underlying object.
    leak_ = this->shared_from_this();
  }

 protected:
  std::shared_ptr<T> leak_;
};

template <typename T>
class ReadValueOperation_ final
    : public Handler,
      public BaseOperation_<ReadValueOperation_<T>> {
 public:
  using callback_t =
      std::function<void(std::shared_ptr<Socket>, const Error& error, T&& t)>;

  ReadValueOperation_(
      std::shared_ptr<Loop> loop,
      std::shared_ptr<Socket> socket,
      callback_t fn)
      : loop_(std::move(loop)), socket_(std::move(socket)), fn_(std::move(fn)) {
    loop_->registerDescriptor(socket_->fd(), EPOLLIN | EPOLLONESHOT, this);
  }
  void handleEvents(int events) override {
    // Move leaked shared_ptr to the stack so that this object
    // destroys itself once this function returns.
    auto self = std::move(this->leak_);

    // Read T.
    auto rv = socket_->read(&t_, sizeof(t_));
    if (rv == -1) {
      fn_(socket_, SystemError("read", errno), std::move(t_));
      return;
    }

    // Check for short read (assume we can read in a single call).
    if (rv < sizeof(t_)) {
      fn_(socket_, ShortReadError(rv, sizeof(t_)), std::move(t_));
      return;
    }

    fn_(socket_, Error::OK, std::move(t_));
  }

 private:
  std::shared_ptr<Loop> loop_;
  std::shared_ptr<Socket> socket_;
  callback_t fn_;

  T t_;
};

template <typename T>
void read(
    std::shared_ptr<Loop> loop,
    std::shared_ptr<Socket> socket,
    typename ReadValueOperation_<T>::callback_t fn) {
  auto x = std::make_shared<ReadValueOperation_<T>>(
      std::move(loop), std::move(socket), std::move(fn));
  x->initialize();
}

template <typename T>
class WriteValueOperation_ final
    : public Handler,
      public BaseOperation_<WriteValueOperation_<T>> {
 public:
  using callback_t =
      std::function<void(std::shared_ptr<Socket>, const Error& error)>;

  WriteValueOperation_(
      std::shared_ptr<Loop> loop,
      std::shared_ptr<Socket> socket,
      T t,
      callback_t fn)
      : loop_(std::move(loop)),
        socket_(std::move(socket)),
        fn_(std::move(fn)),
        t_(std::move(t)) {
    loop_->registerDescriptor(socket_->fd(), EPOLLOUT | EPOLLONESHOT, this);
  }

  void handleEvents(int events) override {
    // Move leaked shared_ptr to the stack so that this object
    // destroys itself once this function returns.
    auto leak = std::move(this->leak_);

    // Write T.
    auto rv = socket_->write(&t_, sizeof(t_));
    if (rv == -1) {
      fn_(socket_, SystemError("write", errno));
      return;
    }

    // Check for short read (assume we can read in a single call).
    if (rv < sizeof(t_)) {
      fn_(socket_, ShortReadError(rv, sizeof(t_)));
      return;
    }

    fn_(socket_, Error::OK);
  }

 private:
  std::shared_ptr<Loop> loop_;
  std::shared_ptr<Socket> socket_;
  callback_t fn_;

  T t_;
};

template <typename T>
void write(
    std::shared_ptr<Loop> loop,
    std::shared_ptr<Socket> socket,
    T t,
    typename WriteValueOperation_<T>::callback_t fn) {
  auto x = std::make_shared<WriteValueOperation_<T>>(
      std::move(loop), std::move(socket), std::move(t), std::move(fn));
  x->initialize();
}

} // namespace tcp
} // namespace transport
} // namespace gloo
