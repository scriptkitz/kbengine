// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "debug_helper.h"
#include "profile.h"
#include "common/common.h"
#include "common/timer.h"
#include "thread/threadguard.h"
#include "network/channel.h"
#include "resmgr/resmgr.h"
#include "network/bundle.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/tcp_packet.h"
#include "server/serverconfig.h"

#if KBE_PLATFORM == PLATFORM_UNIX
#include <unistd.h>
#include <syslog.h>
#endif

#ifndef NO_USE_LOG4CXX
#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "spdlog/common.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/wincolor_sink.h"
#include "spdlog/sinks/ansicolor_sink.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/basic_file_sink.h"
#endif

#include <sys/timeb.h>

namespace KBEngine{
	
KBE_SINGLETON_INIT(DebugHelper);

DebugHelper dbghelper;

#ifndef NO_USE_LOG4CXX
static std::shared_ptr<spdlog::logger> s_cpp_logger;
#endif

#define DBG_PT_SIZE 1024 * 4

#ifdef KBE_USE_ASSERTS
void myassert(const char * exp, const char * func, const char * file, unsigned int line)
{
	DebugHelper::getSingleton().backtrace_msg();
	std::string s = (fmt::format("assertion failed: {}, file {}, line {}, at: {}\n", exp, file, line, func));
	printf("%s%02d: %s", COMPONENT_NAME_EX_2(g_componentType), g_componentGroupOrder, (std::string("[ASSERT]: ") + s).c_str());

	dbghelper.print_msg(s);
    abort();
}
#endif

#if KBE_PLATFORM == PLATFORM_WIN32
	#define ALERT_LOG_TO(NAME, CHANGED)							\
	{															\
		wchar_t exe_path[MAX_PATH];								\
		memset(exe_path, 0, MAX_PATH * sizeof(wchar_t));		\
		GetCurrentDirectory(MAX_PATH, exe_path);				\
																\
		char* ccattr = strutil::wchar2char(exe_path);			\
		if(CHANGED)												\
			printf("Logging(changed) to: %s/logs/" NAME "%s_*.log\n\n", ccattr, COMPONENT_NAME_EX(g_componentType));\
		else													\
			printf("Logging to: %s/logs/" NAME "%s_*.log\n\n", ccattr, COMPONENT_NAME_EX(g_componentType));\
		free(ccattr);											\
	}															\

#else
#define ALERT_LOG_TO(NAME, CHANGED) {}
#endif

//-------------------------------------------------------------------------------------
void utf8printf(FILE *out, const char *str, ...)
{
    va_list ap;
    va_start(ap, str);
    vutf8printf(stdout, str, &ap);
    va_end(ap);
}

//-------------------------------------------------------------------------------------
void vutf8printf(FILE *out, const char *str, va_list* ap)
{
    vfprintf(out, str, *ap);
}

//-------------------------------------------------------------------------------------
DebugHelper::DebugHelper() :
_logfile(NULL),
_currFile(),
_currFuncName(),
_currLine(0),
pNetworkInterface_(NULL),
pDispatcher_(NULL),
#if KBE_PLATFORM == PLATFORM_WIN32
mainThreadID_(GetCurrentThreadId())
#else
mainThreadID_(pthread_self())
#endif
{
}

//-------------------------------------------------------------------------------------
DebugHelper::~DebugHelper()
{
	finalise(true);
}	

//-------------------------------------------------------------------------------------
void DebugHelper::changeLogger(const std::string& name)
{
#ifndef NO_USE_LOG4CXX
	s_cpp_logger = name=="default" ? spdlog::default_logger() : spdlog::get(name);
#endif
}

//-------------------------------------------------------------------------------------
void DebugHelper::initialize(COMPONENT_TYPE componentType)
{
#ifndef NO_USE_LOG4CXX
	
	//packetlogs
	auto packetlogs = spdlog::basic_logger_mt<spdlog::async_factory>("packetlogs", 
		fmt::format("logs/packets/{}.packets.log", COMPONENT_NAME_EX(componentType)), true);
	auto formatter = spdlog::details::make_unique<spdlog::pattern_formatter>("[%T.%e] %L: %v", spdlog::pattern_time_type::local, "");
	packetlogs->set_formatter(std::move(formatter));
	packetlogs->set_level(spdlog::level::debug);

	//logger
#if KBE_PLATFORM == PLATFORM_WIN32
	auto out_sink = std::make_shared<spdlog::sinks::wincolor_stdout_sink_mt>();
#else
	auto out_sink = std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>();
#endif
	auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
		fmt::format("logs/{}.log", COMPONENT_NAME_EX(componentType)), 0, 0);
	const spdlog::sinks_init_list slist = { out_sink, file_sink };

	//s_cpp_logger = std::make_shared<spdlog::async_logger>(COMPONENT_NAME_EX(componentType), slist.begin(), slist.end(), spdlog::thread_pool());
	s_cpp_logger = std::make_shared<spdlog::logger>(COMPONENT_NAME_EX(componentType), slist.begin(), slist.end());
	formatter = spdlog::details::make_unique<spdlog::pattern_formatter>("[%T.%e] %L: %^%v%$", spdlog::pattern_time_type::local, "");
	s_cpp_logger->set_formatter(std::move(formatter));
	s_cpp_logger->set_level(spdlog::level::trace);
	s_cpp_logger->info("\n");
#endif

	ALERT_LOG_TO("", false);
}

//-------------------------------------------------------------------------------------
void DebugHelper::finalise(bool destroy)
{
	//g_kbeSrvConfig.tickMaxBufferedLogs();
	//g_kbeSrvConfig.tickMaxSyncLogs();
}

//-------------------------------------------------------------------------------------
void DebugHelper::pDispatcher(Network::EventDispatcher* dispatcher)
{ 
	pDispatcher_ = dispatcher; 
}

//-------------------------------------------------------------------------------------
void DebugHelper::pNetworkInterface(Network::NetworkInterface* networkInterface)
{ 
	pNetworkInterface_ = networkInterface; 
}

//-------------------------------------------------------------------------------------
void DebugHelper::onMessage(uint32 logType, const char * str, uint32 length)
{
#if !defined( _WIN32 )
	bool isMainThread = (mainThreadID_ == pthread_self());
#else
	bool isMainThread = (mainThreadID_ == GetCurrentThreadId());
#endif

	if(length <= 0 || noSyncLog_)
		return;

	if(g_componentType == MACHINE_TYPE || 
		g_componentType == CONSOLE_TYPE || 
		g_componentType == LOGGER_TYPE || 
		g_componentType == CLIENT_TYPE)
		return;

	if (!isMainThread)
	{
		MemoryStream* pMemoryStream = memoryStreamPool_.createObject(OBJECTPOOL_POINT);

		(*pMemoryStream) << getUserUID();
		(*pMemoryStream) << logType;
		(*pMemoryStream) << g_componentType;
		(*pMemoryStream) << g_componentID;
		(*pMemoryStream) << g_componentGlobalOrder;
		(*pMemoryStream) << g_componentGroupOrder;

		uint64 t = getTimeMs();
		(*pMemoryStream) << (int64)(t / 1000);
		(*pMemoryStream) << (uint32)(t % 1000);
		pMemoryStream->appendBlob(str, length);

		childThreadBufferedLogPackets_.push(pMemoryStream);
	}
	else
	{
		if(g_kbeSrvConfig.tickMaxBufferedLogs() > 0 && hasBufferedLogPackets_ > g_kbeSrvConfig.tickMaxBufferedLogs())
		{
			int8 v = Network::g_trace_packet;
			Network::g_trace_packet = 0;

#ifdef NO_USE_LOG4CXX
#else
			KBE_LOG4CXX_WARN(g_logger, fmt::format("DebugHelper::onMessage: bufferedLogPackets is full({} > kbengine[_defs].xml->logger->tick_max_buffered_logs->{})!\n", 
				hasBufferedLogPackets_, g_kbeSrvConfig.tickMaxBufferedLogs()));
#endif

			Network::g_trace_packet = v;

			clearBufferedLog();
			
#ifdef NO_USE_LOG4CXX
#else
			KBE_LOG4CXX_WARN(g_logger, fmt::format("DebugHelper::onMessage: discard logs!\n"));
#endif
			return;
		}

		int8 trace_packet = Network::g_trace_packet;
		Network::g_trace_packet = 0;
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);

		pBundle->newMessage(LoggerInterface::writeLog);

		(*pBundle) << getUserUID();
		(*pBundle) << logType;
		(*pBundle) << g_componentType;
		(*pBundle) << g_componentID;
		(*pBundle) << g_componentGlobalOrder;
		(*pBundle) << g_componentGroupOrder;

		uint64 t = getTimeMs();
		(*pBundle) << (int64)(t / 1000);
		(*pBundle) << (uint32)(t % 1000);
		pBundle->appendBlob(str, length);

		bufferedLogPackets_.push(pBundle);
		Network::g_trace_packet = trace_packet;
		g_pDebugHelperSyncHandler->startActiveTick();
	}

	++hasBufferedLogPackets_;
}

//-------------------------------------------------------------------------------------
void DebugHelper::print_msg(const std::string& s)
{
#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->info(s);
#endif

	onMessage(KBELOG_PRINT, s.c_str(), (uint32)s.size());
}

//-------------------------------------------------------------------------------------
void DebugHelper::error_msg(const std::string& s)
{
#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->error(s);
#endif

	onMessage(KBELOG_ERROR, s.c_str(), (uint32)s.size());
}

//-------------------------------------------------------------------------------------
void DebugHelper::info_msg(const std::string& s)
{
#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->info(s);
#endif

	onMessage(KBELOG_INFO, s.c_str(), (uint32)s.size());
}

//-------------------------------------------------------------------------------------
int KBELOG_TYPE_MAPPING(int type)
{
#ifdef NO_USE_LOG4CXX
	return KBELOG_SCRIPT_INFO;
#else
	switch(type)
	{
	case spdlog::level::info:
		return KBELOG_SCRIPT_INFO;
	case spdlog::level::err:
		return KBELOG_SCRIPT_ERROR;
	case spdlog::level::debug:
		return KBELOG_SCRIPT_DEBUG;
	case spdlog::level::warn:
		return KBELOG_SCRIPT_WARNING;
	default:
		break;
	}

	return KBELOG_SCRIPT_NORMAL;
#endif
}

//-------------------------------------------------------------------------------------
void DebugHelper::script_info_msg(const std::string& s)
{
#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->info(s);
#endif

	onMessage(KBELOG_SCRIPT_INFO, s.c_str(), (uint32)s.size());
}

//-------------------------------------------------------------------------------------
void DebugHelper::script_error_msg(const std::string& s)
{
#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->error(s);
#endif

	onMessage(KBELOG_SCRIPT_ERROR, s.c_str(), (uint32)s.size());
}

//-------------------------------------------------------------------------------------
void DebugHelper::setScriptMsgType(int msgtype)
{

}

//-------------------------------------------------------------------------------------
void DebugHelper::resetScriptMsgType()
{
	setScriptMsgType(0);
}

//-------------------------------------------------------------------------------------
void DebugHelper::debug_msg(const std::string& s)
{
#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->debug(s);
#endif

	onMessage(KBELOG_DEBUG, s.c_str(), (uint32)s.size());
}

//-------------------------------------------------------------------------------------
void DebugHelper::warning_msg(const std::string& s)
{
#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->warn(s);
#endif

	onMessage(KBELOG_WARNING, s.c_str(), (uint32)s.size());
}

//-------------------------------------------------------------------------------------
void DebugHelper::critical_msg(const std::string& s)
{
	char buf[DBG_PT_SIZE];
	kbe_snprintf(buf, DBG_PT_SIZE, "%s(%d) -> %s\n\t%s\n", _currFile.c_str(), _currLine, _currFuncName.c_str(), s.c_str());

#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->critical(std::string(buf));
#endif

	onMessage(KBELOG_CRITICAL, buf, (uint32)strlen(buf));
	backtrace_msg();
}


//-------------------------------------------------------------------------------------
#if KBE_PLATFORM == PLATFORM_UNIX
#define MAX_DEPTH 50
#include <execinfo.h>
#include <cxxabi.h>

void DebugHelper::backtrace_msg()
{
	void ** traceBuffer = new void*[MAX_DEPTH];
	uint32 depth = backtrace( traceBuffer, MAX_DEPTH );
	char ** traceStringBuffer = backtrace_symbols( traceBuffer, depth );
	for (uint32 i = 0; i < depth; i++)
	{
		// Format: <executable path>(<mangled-function-name>+<function
		// instruction offset>) [<eip>]
		std::string functionName;

		std::string traceString( traceStringBuffer[i] );
		std::string::size_type begin = traceString.find( '(' );
		bool gotFunctionName = (begin >= 0);

		if (gotFunctionName)
		{
			// Skip the round bracket start.
			++begin;
			std::string::size_type bracketEnd = traceString.find( ')', begin );
			std::string::size_type end = traceString.rfind( '+', bracketEnd );
			std::string mangled( traceString.substr( begin, end - begin ) );

			int status = 0;
			size_t demangledBufferLength = 0;
			char * demangledBuffer = abi::__cxa_demangle( mangled.c_str(), 0, 
				&demangledBufferLength, &status );

			if (demangledBuffer)
			{
				functionName.assign( demangledBuffer, demangledBufferLength );

				// __cxa_demangle allocates the memory for the demangled
				// output using malloc(), we need to free it.
				free( demangledBuffer );
			}
			else
			{
				// Didn't demangle, but we did get a function name, use that.
				functionName = mangled;
			}
		}

		std::string ss = fmt::format("Stack: #{} {}\n", 
			i,
			((gotFunctionName) ? functionName.c_str() : traceString.c_str()));

#ifdef NO_USE_LOG4CXX
#else
			s_cpp_logger->info(ss);
#endif
			onMessage(KBELOG_PRINT, ss.c_str(), ss.size());

	}

	free(traceStringBuffer);
	delete[] traceBuffer;
}

#else
void DebugHelper::backtrace_msg()
{
}
#endif

//-------------------------------------------------------------------------------------
void DebugHelper::closeLogger()
{
	// close logger for fork + execv
#ifndef NO_USE_LOG4CXX
	s_cpp_logger->flush();
	s_cpp_logger = NULL;
	spdlog::drop_all();
	spdlog::shutdown();
#endif
}


}


