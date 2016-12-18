#pragma once

#include <memory>
#include <set>
#include <string>

#include <lug/System/Export.hpp>
#include <lug/System/Logger/Common.hpp>
#include <lug/System/Logger/Handler.hpp>
#include <lug/System/Logger/LoggingFacility.hpp>
#include <lug/System/Logger/Message.hpp>
#include <lug/System/Utils.hpp>

namespace lug {
namespace System {

class LUG_SYSTEM_API Logger {
public:
    Logger(const std::string& loggerName);
    virtual ~Logger() = default;

    void addHandler(Handler* handler);

    void addHandler(const std::string& name);

    void defaultErrHandler(const std::string& msg);

    template<typename T>
    void log(Level::enumLevel lvl, const T& msg);

    template<typename... Args, typename T>
    void log(Level::enumLevel lvl, const T& fmt, const Args &... args);

    template<typename T, typename... Args>
    void debug(const T& fmt, const Args&... args);

    template<typename T, typename... Args>
    void info(const T& fmt, const Args&... args);

    template<typename T, typename... Args>
    void warn(const T& fmt, const Args&... args);

    template<typename T, typename... Args>
    void error(const T& fmt, const Args&... args);

    template<typename T, typename... Args>
    void fatal(const T& fmt, const Args&... args);

    template<typename T, typename... Args>
    void assrt(const T& fmt, const Args&... args);

    const std::string& getName() const;

    void handle(priv::Message& msg);

    void flush();

    static System::Logger logger;

protected:
    const std::string _name;
    std::set<Handler*> _handlers;
};

#include <lug/System/Logger.inl>

} // namespace System
} // namespace lug
