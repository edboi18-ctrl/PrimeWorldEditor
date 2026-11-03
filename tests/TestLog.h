#ifndef PWE_TESTS_TESTLOG_H
#define PWE_TESTS_TESTLOG_H

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

/** NLog writes through the spdlog logger "axio", which the editor creates in NLog::InitLog (with a
 *  log file). Tests log to stderr only. */
inline void InitTestLog()
{
    if (!spdlog::get("axio"))
        spdlog::stderr_color_mt("axio");
}

#endif // PWE_TESTS_TESTLOG_H
