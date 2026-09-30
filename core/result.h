#pragma once
// Lightweight error handling: Status and Result<T>.

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace avc {

class Status {
public:
    Status() = default;
    static Status ok() { return {}; }
    static Status error(std::string msg) {
        Status s;
        s.ok_ = false;
        s.message_ = std::move(msg);
        return s;
    }
    [[nodiscard]] bool isOk() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    bool ok_ = true;
    std::string message_;
};

template <typename T>
class Result {
public:
    Result(T value) : v_(std::move(value)) {}  // NOLINT(implicit)
    Result(Status err) : v_(std::move(err)) {}  // NOLINT(implicit)
    static Result error(std::string msg) { return Result(Status::error(std::move(msg))); }

    [[nodiscard]] bool isOk() const noexcept { return std::holds_alternative<T>(v_); }
    explicit operator bool() const noexcept { return isOk(); }
    T& value() & { return std::get<T>(v_); }
    const T& value() const& { return std::get<T>(v_); }
    T&& value() && { return std::get<T>(std::move(v_)); }
    T* operator->() { return &std::get<T>(v_); }
    const T* operator->() const { return &std::get<T>(v_); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    [[nodiscard]] Status status() const { return isOk() ? Status::ok() : std::get<Status>(v_); }
    [[nodiscard]] std::string errorMessage() const { return isOk() ? std::string() : std::get<Status>(v_).message(); }

private:
    std::variant<T, Status> v_;
};

}  // namespace avc
