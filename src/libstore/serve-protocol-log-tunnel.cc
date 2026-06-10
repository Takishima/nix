#include "nix/store/serve-protocol-log-tunnel.hh"
#include "nix/store/worker-protocol.hh" // for the STDERR_* log-tunnel framing
#include "nix/util/util.hh"

#include <sstream>

namespace nix {

/* Serialize logger fields the same way the worker-protocol stderr tunnel does
   (matches `readServeLogFields` on the client). */
static void writeServeLogFields(Sink & to, const Logger::Fields & fields)
{
    to << fields.size();
    for (auto & f : fields) {
        to << (uint64_t) f.type;
        if (f.type == Logger::Field::tInt)
            to << f.i;
        else if (f.type == Logger::Field::tString)
            to << f.s;
        else
            unreachable();
    }
}

void ServeTunnelLogger::enqueue(std::string s)
{
    if (canSend) {
        to(s);
        to.flush();
    } else
        pending.push_back(std::move(s));
}

void ServeTunnelLogger::log(Verbosity lvl, std::string_view s)
{
    if (lvl > verbosity)
        return;
    StringSink buf;
    buf << STDERR_NEXT << (std::string(s) + "\n");
    enqueue(std::move(buf.s));
}

void ServeTunnelLogger::logEI(const ErrorInfo & ei)
{
    if (ei.level > verbosity)
        return;
    std::ostringstream oss;
    showErrorInfo(oss, ei, false);
    StringSink buf;
    buf << STDERR_NEXT << oss.view();
    enqueue(std::move(buf.s));
}

void ServeTunnelLogger::startActivity(
    ActivityId act, Verbosity lvl, ActivityType type, const std::string & s, const Fields & fields, ActivityId parent)
{
    StringSink buf;
    buf << STDERR_START_ACTIVITY << act << (uint64_t) lvl << (uint64_t) type << s;
    writeServeLogFields(buf, fields);
    buf << parent;
    enqueue(std::move(buf.s));
}

void ServeTunnelLogger::stopActivity(ActivityId act)
{
    StringSink buf;
    buf << STDERR_STOP_ACTIVITY << act;
    enqueue(std::move(buf.s));
}

void ServeTunnelLogger::result(ActivityId act, ResultType type, const Fields & fields)
{
    StringSink buf;
    buf << STDERR_RESULT << act << (uint64_t) type;
    writeServeLogFields(buf, fields);
    enqueue(std::move(buf.s));
}

void ServeTunnelLogger::startWork()
{
    canSend = true;
    for (auto & msg : pending)
        to(msg);
    pending.clear();
    to.flush();
}

void ServeTunnelLogger::stopWork()
{
    canSend = false;
    to << STDERR_LAST;
    to.flush();
}

} // namespace nix
