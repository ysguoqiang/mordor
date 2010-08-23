#ifndef __MORDOR_LOG_H__
#define __MORDOR_LOG_H__
// Copyright (c) 2009 - Mozy, Inc.

#include <list>
#include <set>
#include <sstream>

#include "predef.h"
#include <boost/enable_shared_from_this.hpp>
#include <boost/function.hpp>
#include <boost/noncopyable.hpp>
#include <boost/date_time/posix_time/ptime.hpp>
#include <boost/shared_ptr.hpp>

// For tid_t
#include "thread.h"
#include "version.h"

#ifdef WINDOWS
#include <windows.h>
#endif

namespace Mordor {

class Logger;
class LogSink;

class LoggerIterator;

class Stream;

#ifdef DEBUG
#undef DEBUG
#define DEBUG DEBUG
#endif

/// Static class to gain access to and configure global logger instances

/// The logging framework is made up of three main classes: Log, Logger, and
/// LogSink.  LogSinks are where log messages go.  Loggers are used to break
/// up the logging into logical units, and are set up in a hierarchy.  Every
/// Logger has a name that implies its location in the hieararchy: The name
/// is split on semicolons, and it is a child Logger of the Logger with
/// everything except the last component.  Intermediate loggers are implicitly
/// created, if necessary.  When you add a LogSink to a Logger, all messages
/// generated by that logger and all of its children are sent to that LogSink.
/// Each Logger has it's own log Level that it is enabled for.  Each higher
/// level is a superset of the prior level.  So, for example, the root logger
/// can have a StdoutLogSink, and set for DEBUG level, and a logger named
/// mordor:http:client can have a FileLogSink and be set for WARNING level.
/// In that case, a log message sent to mordor:http:client at INFO level would
/// be dropped, but at ERROR level would go to both the file, and stdout (that
/// it inherited from the root logger).  A message sent to mordor:streams:ssl
/// at DEBUG level would only go to stdout.
///
/// By default, all Loggers have no LogSinks, and are set at INFO level.
///
/// In practice, it is easiest to use the log.* ConfigVars to configure
/// logging.  There is a ConfigVar for each standard LogSink to enable it (on
/// the root Logger), and a ConfigVar for each log level.  Each log level
/// ConfigVar is a regex of which Loggers should be enabled at that level.  A
/// Logger will be set to the highest level that it matches: i.e. if
/// log.debugmask=mordor:http:client, and log.tracemask=.*, mordor:http:client
/// will be set to TRACE.  Note that if any of the log level ConfigVars change,
/// it will wipe out any manually configured logging levels (so is incompatible
/// with manually managing log levels).  The log sink ConfigVars are not
/// incompatible with manually managing your own LogSinks.

/// @sa LogMacros
class Log
{
private:
    Log();

public:
    /// The level of a log message
    enum Level {
        NONE,

        /// The application cannot continue
        FATAL,

        /// An error occurred; it cannot locally be recovered from, but may be
        /// recoverable from a more general context
        ERROR,

        /// An error occured that was ignored/recovered, but may be useful
        /// to know the error happened
        WARNING,

        /// A normal, but significant event occurred
        INFO,

        /// A somewhat significant event occurred
        VERBOSE,

        /// Normally only useful for debugging, logs most calls a component
        /// makes
        DEBUG,

        /// Normally only useful for debugging, logs everything under the sun,
        /// including every call a component makes, and possibly details about
        /// internal state
        TRACE,
    };

    /// Find (or create) a logger with the specified name
    static boost::shared_ptr<Logger> lookup(const std::string &name);

    /// Call dg for each registered Logger.
    ///
    /// This may include implicitly created intermediate loggers.
    static void visit(boost::function<void (boost::shared_ptr<Logger>)> dg);

    /// Return the root of the Logger hierarchy
    static boost::shared_ptr<Logger> root();
};

/// Abstract base class for receiving log messages
/// @sa Log
class LogSink
{
    friend class Logger;
public:
    typedef boost::shared_ptr<LogSink> ptr;
public:

    virtual ~LogSink() {}
    /// @brief Receives details of a single log message
    /// @param logger The Logger that generated the message
    /// @param now The timestamp when the message was generated
    /// @param elapsed Microseconds since the process started when the message
    /// was generated
    /// @param thread The id of the thread that generated the message
    /// @param fiber An opaque pointer to the fiber that generated the message
    /// @param level The level of the message
    /// @param str The log message itself
    /// @param file The source file where the message was generated
    /// @param line The source line where the message was generated
    virtual void log(const std::string &logger,
        boost::posix_time::ptime now, unsigned long long elapsed,
        tid_t thread, void *fiber,
        Log::Level level, const std::string &str,
        const char *file, int line) = 0;
};

/// A LogSink that dumps message to stdout (std::cout)
class StdoutLogSink : public LogSink
{
public:
    void log(const std::string &logger,
        boost::posix_time::ptime now, unsigned long long elapsed,
        tid_t thread, void *fiber,
        Log::Level level, const std::string &str,
        const char *file, int line);
};

#ifdef WINDOWS
/// A LogSink that dumps messages to the Visual Studio Debug Output Window
///
/// Using OutputDebugString
class DebugLogSink : public LogSink
{
public:
    void log(const std::string &logger,
        boost::posix_time::ptime now, unsigned long long elapsed,
        tid_t thread, void *fiber,
        Log::Level level, const std::string &str,
        const char *file, int line);
};
#else
/// A LogSink that sends messages to syslog
class SyslogLogSink : public LogSink
{
public:
    /// @param facility The facility to mark messages with
    SyslogLogSink(int facility);

    void log(const std::string &logger,
        boost::posix_time::ptime now, unsigned long long elapsed,
        tid_t thread, void *fiber,
        Log::Level level, const std::string &str,
        const char *file, int line);

    int facility() const { return m_facility; }

    static int facilityFromString(const char *str);
    static const char *facilityToString(int facility);

private:
    int m_facility;
};
#endif

/// A LogSink that appends messages to a file
///
/// The file is opened in append mode, so multiple processes and threads can
/// log to the same file simultaneously, without fear of corrupting each
/// others' messages.  The messages will still be intermingled, but each one
/// will be atomic
class FileLogSink : public LogSink
{
public:
    /// @param file The file to open and log to.  If it does not exist, it is
    /// created.
    FileLogSink(const std::string &file);

    void log(const std::string &logger,
        boost::posix_time::ptime now, unsigned long long elapsed,
        tid_t thread, void *fiber,
        Log::Level level, const std::string &str,
        const char *file, int line);

    std::string file() const { return m_file; }

private:
    std::string m_file;
    boost::shared_ptr<Stream> m_stream;
};

/// LogEvent is an intermediary class.  It is returned by Logger::log, owns a
/// std::ostream, and on destruction it will log whatever was streamed to it.
/// It *is* copyable, because it is returned from Logger::log, but shouldn't
/// be copied, because when it destructs it will log whatever was built up so
/// far, in addition to the copy logging it.
struct LogEvent
{
    friend class Logger;
private:
    LogEvent(boost::shared_ptr<Logger> logger, Log::Level level,
        const char *file, int line)
        : m_logger(logger),
          m_level(level),
          m_file(file),
          m_line(line)
    {}

public:
    LogEvent(const LogEvent &copy)
        : m_logger(copy.m_logger),
          m_level(copy.m_level),
          m_file(copy.m_file),
          m_line(copy.m_line)
    {}

    ~LogEvent();
    std::ostream &os() { return m_os; }

private:
    boost::shared_ptr<Logger> m_logger;
    Log::Level m_level;
    const char *m_file;
    int m_line;
    std::ostringstream m_os;
};

/// Temporarily disables logging for this Fiber
struct LogDisabler
{
    LogDisabler();
    ~LogDisabler();

private:
    bool m_disabled;
};

struct LoggerLess
{
    bool operator()(const boost::shared_ptr<Logger> &lhs,
        const boost::shared_ptr<Logger> &rhs) const;
};

/// An individual Logger.
/// @sa Log
/// @sa LogMacros
class Logger : public boost::enable_shared_from_this<Logger>
{
    friend class Log;
    friend struct LoggerLess;
public:
    typedef boost::shared_ptr<Logger> ptr;
private:
    Logger();
    Logger(const std::string &name, Logger::ptr parent);

public:
    /// @return If this logger is enabled at level
    bool enabled(Log::Level level);
    /// Set this logger to level
    /// @param level The level to set it to
    /// @param propagate Automatically set all child Loggers to this level also
    void level(Log::Level level, bool propagate = true);
    /// @return The current level this Logger is set to
    Log::Level level() const { return m_level; }

    /// @return If this logger will inherit LogSinks from its parent
    bool inheritSinks() const { return m_inheritSinks; }
    /// Set if this logger will inherit LogSinks from its parent
    void inheritSinks(bool inherit) { m_inheritSinks = inherit; }
    /// Add sink to this Logger
    void addSink(LogSink::ptr sink) { m_sinks.push_back(sink); }
    /// Remove sink from this Logger
    void removeSink(LogSink::ptr sink);
    /// Remove all LogSinks from this logger
    void clearSinks() { m_sinks.clear(); }

    /// Return a LogEvent to use to stream a log message applicable to this
    /// Logger
    /// @param level The level this message will be
    LogEvent log(Log::Level level, const char *file = NULL, int line = -1)
    { return LogEvent(shared_from_this(), level, file, line); }
    /// Log a message from this Logger
    /// @param level The level of this message
    /// @param str The message
    void log(Log::Level level, const std::string &str, const char *file = NULL, int line = 0);

    /// @return The full name of this Logger
    std::string name() const { return m_name; }

private:
    std::string m_name;
    boost::weak_ptr<Logger> m_parent;
    std::set<Logger::ptr, LoggerLess> m_children;
    Log::Level m_level;
    std::list<LogSink::ptr> m_sinks;
    bool m_inheritSinks;
};

/// @defgroup LogMacros Logging Macros
/// Macros that automatically capture the current file and line, and return
/// a std::ostream & to stream the log message to.  Note that it is *not* an
/// rvalue, because it is put inside an un-scoped if statement, so that the
/// entire streaming of the log statement can be skipped if the Logger is not
/// enabled at the specified level.
/// @sa Log
/// @{

/// @brief Log at a particular level
/// @param level The level to log at
#define MORDOR_LOG_LEVEL(lg, level) if ((lg)->enabled(level))                   \
    (lg)->log(level, __FILE__, __LINE__).os()
/// Log a fatal error
#define MORDOR_LOG_FATAL(log) MORDOR_LOG_LEVEL(log, ::Mordor::Log::FATAL)
/// Log an error
#define MORDOR_LOG_ERROR(log) MORDOR_LOG_LEVEL(log, ::Mordor::Log::ERROR)
/// Log a warning
#define MORDOR_LOG_WARNING(log) MORDOR_LOG_LEVEL(log, ::Mordor::Log::WARNING)
/// Log an informational message
#define MORDOR_LOG_INFO(log) MORDOR_LOG_LEVEL(log, ::Mordor::Log::INFO)
/// Log a verbose message
#define MORDOR_LOG_VERBOSE(log) MORDOR_LOG_LEVEL(log, ::Mordor::Log::VERBOSE)
/// Log a debug message
#define MORDOR_LOG_DEBUG(log) MORDOR_LOG_LEVEL(log, ::Mordor::Log::DEBUG)
/// Log a trace message
#define MORDOR_LOG_TRACE(log) MORDOR_LOG_LEVEL(log, ::Mordor::Log::TRACE)
/// @}

/// Streams a Log::Level as a string, instead of an integer
std::ostream &operator <<(std::ostream &os, Mordor::Log::Level level);
#ifdef WINDOWS
/// Streams a Log::Level as a string, instead of an integer
std::wostream &operator <<(std::wostream &os, Mordor::Log::Level level);
#endif

}


#endif
