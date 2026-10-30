#pragma once

#include <stdexcept>
#include <string>
#include <vector>
#include <memory>

class DiagnosisException : public std::runtime_error
{
    using Args = std::vector<std::string>;

public:
    explicit DiagnosisException(std::string msg, Args msgFormat = {})
        : std::runtime_error(msg),
        msgFormat(std::move(msgFormat))
    {
    }

    DiagnosisException(std::string msg,
        std::exception_ptr cause,
        Args msgFormat = {})
        : std::runtime_error(msg),
        cause(std::move(cause)),
        msgFormat(std::move(msgFormat))
    {
    }

    const std::exception_ptr& GetCause() const noexcept
    {
        return cause;
    }

    const Args& GetFormatArgs() const noexcept
    {
        return msgFormat;
    }
private:
    std::exception_ptr cause;
    Args msgFormat;
};