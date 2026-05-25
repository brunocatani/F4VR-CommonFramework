#pragma once

// ReSharper disable CppUnnamedNamespaceInHeaderFile
// ReSharper disable CppClangTidyBugproneMacroParentheses

#include <algorithm>
#include <chrono>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <unordered_map>

namespace fs = std::filesystem;

#pragma once

#define MAKE_SOURCE_LOGGER(log_func, log_level)                                                                                                    \
                                                                                                                                                   \
    template <class... Args>                                                                                                                       \
    struct [[maybe_unused]] log_func                                                                                                               \
    {                                                                                                                                              \
        log_func() = delete;                                                                                                                       \
                                                                                                                                                   \
        explicit log_func(spdlog::format_string_t<Args...> fmt, Args&&... args, const std::source_location& loc = std::source_location::current()) \
        {                                                                                                                                          \
            spdlog::source_loc sourceLoc{ loc.file_name(), static_cast<int>(loc.line()), loc.function_name() };                                    \
            internal::_logger->log(sourceLoc, spdlog::level::log_level, fmt, std::forward<Args>(args)...);                                         \
        }                                                                                                                                          \
    };                                                                                                                                             \
                                                                                                                                                   \
    template <class... Args>                                                                                                                       \
    log_func(spdlog::format_string_t<Args...>, Args&&...) -> log_func<Args...>;

namespace f4cf::logger::internal
{
    class HybridFormatter;

    static constexpr auto RAW_LOGGER_NAME = "RAW"sv;

    /**
     * Global logger instance
     */
    inline std::shared_ptr<spdlog::logger> _logger;

    /**
     * Logger for raw logging (without the log pattern)
     */
    inline std::shared_ptr<spdlog::logger> _rawLogger;

    /**
     * Current log level
     */
    inline int _logLevel = -1;

    /**
     * Default log pattern: full date, bracketed level and the logging class (see ClassNameFlag).
     * Used when the config gives no pattern or an empty one.
     */
    inline constexpr auto DEFAULT_LOG_PATTERN = "%Y-%m-%d %H:%M:%S.%e [%l] [%-20!k] %v";

    /**
     * Current global log pattern
     */
    inline std::string _logPattern = DEFAULT_LOG_PATTERN;

    /**
     * Holds the last time of a log message per key.
     */
    inline std::unordered_map<std::string_view, std::chrono::steady_clock::time_point> _sampleMessagesTtl;

    constexpr int clampLogLevel(const int logLevel)
    {
        return std::clamp(logLevel, static_cast<int>(spdlog::level::trace), static_cast<int>(spdlog::level::off));
    }

    constexpr const char* logLevelName(const int logLevel)
    {
        switch (static_cast<spdlog::level::level_enum>(clampLogLevel(logLevel))) {
        case spdlog::level::trace:
            return "trace";
        case spdlog::level::debug:
            return "debug";
        case spdlog::level::info:
            return "info";
        case spdlog::level::warn:
            return "warn";
        case spdlog::level::err:
            return "error";
        case spdlog::level::critical:
            return "critical";
        case spdlog::level::off:
            return "off";
        default:
            return "unknown";
        }
    }

    /**
     * Logs at "level" but only one message per "time" milliseconds per format string, other logs are dropped.
     */
    template <class... Args>
    void sampleImpl(const spdlog::level::level_enum level, const int time, const std::source_location& loc, spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        if (!_logger || !_logger->should_log(level)) {
            return;
        }

        const fmt::basic_string_view<char> fmtView = fmt;
        const std::string_view key(fmtView.data(), fmtView.size());

        const auto now = std::chrono::steady_clock::now();
        if (_sampleMessagesTtl.contains(key) && now - _sampleMessagesTtl[key] <= std::chrono::milliseconds(time)) {
            return;
        }

        _sampleMessagesTtl[key] = now;
        std::string formatted = fmt::format(fmt, std::forward<Args>(args)...);
        const spdlog::source_loc sourceLoc{ loc.file_name(), static_cast<int>(loc.line()), loc.function_name() };
        _logger->log(sourceLoc, level, "[sample] {}", formatted);
    }

    /**
     * Pattern flag character for the class name, see ClassNameFlag. Not one of spdlog's own flags.
     */
    static constexpr char CLASS_NAME_FLAG = 'k';

    /**
     * Pattern flag that prints the source file name without directory or extension, which by the
     * one-class-per-file convention is the name of the class that logged the message.
     * spdlog's own "%s" is the closest built-in but always keeps the ".cpp".
     *
     * Padding is applied by hand because spdlog's scoped_padder lives in the header-only inline
     * file and is not available when linking against the compiled library.
     */
    class ClassNameFlag : public spdlog::custom_flag_formatter
    {
    public:
        void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override
        {
            std::string_view name = classNameOf(msg.source.filename);
            if (padinfo_.truncate_ && padinfo_.width_ > 0 && name.size() > padinfo_.width_) {
                name = name.substr(0, padinfo_.width_);
            }

            const auto pad = padinfo_.width_ > name.size() ? padinfo_.width_ - name.size() : 0;
            const auto padBefore = padinfo_.side_ == spdlog::details::padding_info::pad_side::left     ? pad
                                   : padinfo_.side_ == spdlog::details::padding_info::pad_side::center ? pad / 2
                                                                                                       : 0;

            appendSpaces(dest, padBefore);
            dest.append(name.data(), name.data() + name.size());
            appendSpaces(dest, pad - padBefore);
        }

        virtual std::unique_ptr<spdlog::custom_flag_formatter> clone() const override
        {
            // padding is re-applied by pattern_formatter via set_padding_info after cloning
            return std::make_unique<ClassNameFlag>();
        }

    private:
        static std::string_view classNameOf(const char* sourceFilePath)
        {
            if (!sourceFilePath) {
                return {};
            }
            std::string_view name(sourceFilePath);
            if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos) {
                name.remove_prefix(slash + 1);
            }
            if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos) {
                name = name.substr(0, dot);
            }
            return name;
        }

        static void appendSpaces(spdlog::memory_buf_t& dest, const size_t count)
        {
            // spdlog caps pattern padding at 64, so one append always covers it
            static constexpr std::string_view SPACES = "                                                                "sv;
            const auto clamped = std::min(count, SPACES.size());
            dest.append(SPACES.data(), SPACES.data() + clamped);
        }
    };

    /**
     * Custom formatter used to be able to format dump messages without the pattern prefix
     */
    class HybridFormatter : public spdlog::formatter
    {
    public:
        HybridFormatter()
        {
            auto formatter = std::make_unique<spdlog::pattern_formatter>();
            formatter->add_flag<ClassNameFlag>(CLASS_NAME_FLAG);
            formatter->set_pattern(_logPattern);
            inner = std::move(formatter);
        }

        virtual std::unique_ptr<formatter> clone() const override
        {
            return std::make_unique<HybridFormatter>();
        }

        virtual void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override
        {
            if (msg.logger_name == RAW_LOGGER_NAME) {
                dest.append(msg.payload);
                dest.append("\n"sv);
            } else {
                inner->format(msg, dest);
            }
        }

    private:
        std::unique_ptr<spdlog::pattern_formatter> inner;
    };
}

namespace f4cf::logger
{
    MAKE_SOURCE_LOGGER(trace, trace);
    MAKE_SOURCE_LOGGER(debug, debug);
    MAKE_SOURCE_LOGGER(info, info);
    MAKE_SOURCE_LOGGER(warn, warn);
    MAKE_SOURCE_LOGGER(error, err);
    MAKE_SOURCE_LOGGER(critical, critical);

    inline bool isTraceEnabled()
    {
        return internal::_logger && internal::_logger->should_log(spdlog::level::trace);
    }

    inline bool isDebugEnabled()
    {
        return internal::_logger && internal::_logger->should_log(spdlog::level::debug);
    }

    inline bool isInfoEnabled()
    {
        return internal::_logger && internal::_logger->should_log(spdlog::level::info);
    }

    inline bool isWarnEnabled()
    {
        return internal::_logger && internal::_logger->should_log(spdlog::level::warn);
    }

    inline bool isErrorEnabled()
    {
        return internal::_logger && internal::_logger->should_log(spdlog::level::err);
    }

    inline bool isCriticalEnabled()
    {
        return internal::_logger && internal::_logger->should_log(spdlog::level::critical);
    }

    /**
     * Same as calling info() but only one message log per "time" in milliseconds, other logs are dropped.
     * Use the message format as a key to identify the log messages that should be sampled.
     * Defaults to one message per second, pass "time" as the first argument to override it.
     *
     * A struct with deduction guides rather than a function, for the same reason as the level loggers
     * above: the source location has to be a defaulted parameter after the variadic arguments, which
     * only works once the pack is fixed by class template argument deduction.
     */
    template <class... Args>
    struct [[maybe_unused]] sample
    {
        sample() = delete;

        explicit sample(spdlog::format_string_t<Args...> fmt, Args&&... args, const std::source_location& loc = std::source_location::current())
        {
            internal::sampleImpl(spdlog::level::info, 1000, loc, fmt, std::forward<Args>(args)...);
        }

        explicit sample(const int time, spdlog::format_string_t<Args...> fmt, Args&&... args, const std::source_location& loc = std::source_location::current())
        {
            internal::sampleImpl(spdlog::level::info, time, loc, fmt, std::forward<Args>(args)...);
        }
    };

    template <class... Args>
    sample(spdlog::format_string_t<Args...>, Args&&...) -> sample<Args...>;

    template <class... Args>
    sample(int, spdlog::format_string_t<Args...>, Args&&...) -> sample<Args...>;

    /**
     * Same as sample() but logged at the debug level, so only when it is enabled.
     */
    template <class... Args>
    struct [[maybe_unused]] sampleDebug
    {
        sampleDebug() = delete;

        explicit sampleDebug(spdlog::format_string_t<Args...> fmt, Args&&... args, const std::source_location& loc = std::source_location::current())
        {
            internal::sampleImpl(spdlog::level::debug, 1000, loc, fmt, std::forward<Args>(args)...);
        }

        explicit sampleDebug(const int time, spdlog::format_string_t<Args...> fmt, Args&&... args, const std::source_location& loc = std::source_location::current())
        {
            internal::sampleImpl(spdlog::level::debug, time, loc, fmt, std::forward<Args>(args)...);
        }
    };

    template <class... Args>
    sampleDebug(spdlog::format_string_t<Args...>, Args&&...) -> sampleDebug<Args...>;

    template <class... Args>
    sampleDebug(int, spdlog::format_string_t<Args...>, Args&&...) -> sampleDebug<Args...>;

    /**
     * Same as sample() but logged at the warn level.
     */
    template <class... Args>
    struct [[maybe_unused]] sampleWarn
    {
        sampleWarn() = delete;

        explicit sampleWarn(spdlog::format_string_t<Args...> fmt, Args&&... args, const std::source_location& loc = std::source_location::current())
        {
            internal::sampleImpl(spdlog::level::warn, 1000, loc, fmt, std::forward<Args>(args)...);
        }

        explicit sampleWarn(const int time, spdlog::format_string_t<Args...> fmt, Args&&... args, const std::source_location& loc = std::source_location::current())
        {
            internal::sampleImpl(spdlog::level::warn, time, loc, fmt, std::forward<Args>(args)...);
        }
    };

    template <class... Args>
    sampleWarn(spdlog::format_string_t<Args...>, Args&&...) -> sampleWarn<Args...>;

    template <class... Args>
    sampleWarn(int, spdlog::format_string_t<Args...>, Args&&...) -> sampleWarn<Args...>;

    template <class... Args>
    void infoRaw(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        internal::_rawLogger->log(spdlog::level::info, fmt, std::forward<Args>(args)...);
    }

    /**
     * Init logging using a log with the given name put in "My Games" folder.
     */
    inline void init(const std::string_view& logFileName)
    {
        auto path = F4SE::log::log_directory();
        const auto gamepath = REL::Module::IsVR() ? "Fallout4VR/F4SE" : "Fallout4/F4SE";
        if (!path.value().generic_string().ends_with(gamepath)) {
            // handle bug where game directory is missing
            path = path.value().parent_path().append(gamepath);
        }

        // Use rolling log files (5 files, max 10mb each)
        *path /= fmt::format("{}.log"sv, logFileName);
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path->string(), 1024 * 1024 * 10, 5, true);

        internal::_logger = std::make_shared<spdlog::logger>("GLOBAL"s, sink);
        internal::_logger->set_level(spdlog::level::info);
        internal::_logger->flush_on(spdlog::level::info);
        internal::_logger->set_formatter(std::make_unique<internal::HybridFormatter>());
        spdlog::set_default_logger(internal::_logger);

        internal::_rawLogger = std::make_shared<spdlog::logger>(std::string(internal::RAW_LOGGER_NAME), sink);
        internal::_rawLogger->set_level(spdlog::level::info);
        internal::_rawLogger->flush_on(spdlog::level::info);
    }

    /**
     * Update the global logger log level based on the config setting.
     */
    inline void setLogLevelAndPattern(int logLevel, const std::string& logPattern)
    {
        const int clampedLogLevel = internal::clampLogLevel(logLevel);
        if (internal::_logLevel != clampedLogLevel) {
            info("Set log level = {} ({})", clampedLogLevel, internal::logLevelName(clampedLogLevel));
            internal::_logLevel = clampedLogLevel;
            const auto levelEnum = static_cast<spdlog::level::level_enum>(clampedLogLevel);
            internal::_logger->set_level(levelEnum);
            internal::_logger->flush_on(levelEnum);
            internal::_rawLogger->set_level(levelEnum);
            internal::_rawLogger->flush_on(levelEnum);
        }

        // see: https://github.com/gabime/spdlog/wiki/Custom-formatting
        const std::string normalizedPattern = logPattern.empty() ? std::string(internal::DEFAULT_LOG_PATTERN) : logPattern;
        if (internal::_logPattern != normalizedPattern) {
            info("Set log pattern = {}", normalizedPattern);
            internal::_logPattern = normalizedPattern;
            spdlog::set_formatter(std::make_unique<internal::HybridFormatter>());
        }
    }
}

#undef MAKE_SOURCE_LOGGER
