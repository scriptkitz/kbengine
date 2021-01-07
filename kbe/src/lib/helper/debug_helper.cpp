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
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/basic_file_sink.h"
#endif

#include <sys/timeb.h>

#include "../../server/tools/logger/logger_interface.h"

namespace KBEngine{
	
KBE_SINGLETON_INIT(DebugHelper);

DebugHelper dbghelper;
ProfileVal g_syncLogProfile("syncLog");

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
class DebugHelperSyncHandler  : public TimerHandler
{
public:
	DebugHelperSyncHandler():
	pActiveTimerHandle_(NULL)
	{
	}

	virtual ~DebugHelperSyncHandler()
	{
		// cancel();
	}

	enum TimeOutType
	{
		TIMEOUT_ACTIVE_TICK,
		TIMEOUT_MAX
	};

	virtual void handleTimeout(TimerHandle handle, void * arg)
	{
		g_syncLogProfile.start();
		DebugHelper::getSingleton().sync();
		g_syncLogProfile.stop();
	}

	//-------------------------------------------------------------------------------------
	void cancel()
	{
		if(pActiveTimerHandle_ == NULL)
			return;

		pActiveTimerHandle_->cancel();
		delete pActiveTimerHandle_;
		pActiveTimerHandle_ = NULL;
	}

	//-------------------------------------------------------------------------------------
	void startActiveTick()
	{
		if(pActiveTimerHandle_ == NULL)
		{
			if(DebugHelper::getSingleton().pDispatcher())
			{
				pActiveTimerHandle_ = new TimerHandle();
				(*pActiveTimerHandle_) = DebugHelper::getSingleton().pDispatcher()->addTimer(1000000 / 10,
												this, (void *)TIMEOUT_ACTIVE_TICK);
			}
		}
	}

private:
	TimerHandle* pActiveTimerHandle_;
};

DebugHelperSyncHandler* g_pDebugHelperSyncHandler = NULL;

//-------------------------------------------------------------------------------------
DebugHelper::DebugHelper() :
_logfile(NULL),
_currFile(),
_currFuncName(),
_currLine(0),
loggerAddr_(),
logMutex(),
bufferedLogPackets_(),
hasBufferedLogPackets_(0),
pNetworkInterface_(NULL),
pDispatcher_(NULL),
scriptMsgType_((int)spdlog::level::info),
noSyncLog_(false),
canLogFile_(true),
loseLoggerTime_(timestamp()),

#if KBE_PLATFORM == PLATFORM_WIN32
mainThreadID_(GetCurrentThreadId()),
#else
mainThreadID_(pthread_self()),
#endif
memoryStreamPool_("DebugHelperMemoryStream")
{
	g_pDebugHelperSyncHandler = new DebugHelperSyncHandler();
	loseLoggerTime_ = timestamp();
}

//-------------------------------------------------------------------------------------
DebugHelper::~DebugHelper()
{
	finalise(true);
}	

//-------------------------------------------------------------------------------------
std::string DebugHelper::getLogName()
{
#ifndef NO_USE_LOG4CXX
	/*
	log4cxx::FileAppenderPtr appender = (log4cxx::FileAppenderPtr)g_logger->getAppender(log4cxx::LogString(L"R"));
	if(appender == NULL || appender->getFile().size() == 0)
		return "";

	char* ccattr = strutil::wchar2char(appender->getFile().c_str());
	std::string path = ccattr;
	free(ccattr);

	return path;
	*/
#endif

	return "";
}

//-------------------------------------------------------------------------------------
void DebugHelper::changeLogger(const std::string& name)
{
#ifndef NO_USE_LOG4CXX
	s_cpp_logger = name=="default" ? spdlog::default_logger() : spdlog::get(name);
#endif
}

//-------------------------------------------------------------------------------------
bool DebugHelper::canLog(int level)
{
#ifndef NO_USE_LOG4CXX
	if (s_cpp_logger->level() != spdlog::level::off)
		return true;
#else
	return true;
#endif
}

//-------------------------------------------------------------------------------------
void DebugHelper::lockthread()
{
	logMutex.lockMutex();
}

//-------------------------------------------------------------------------------------
void DebugHelper::unlockthread()
{
	logMutex.unlockMutex();
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
	auto out_sink = std::make_shared<spdlog::sinks::stdout_sink_st>();
	auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_st>(
		fmt::format("logs/{}.log", COMPONENT_NAME_EX(componentType)), 0, 0);
	const spdlog::sinks_init_list slist = { out_sink, file_sink };

	s_cpp_logger = std::make_shared<spdlog::logger>(COMPONENT_NAME_EX(componentType), slist.begin(), slist.end());
	formatter = spdlog::details::make_unique<spdlog::pattern_formatter>("[%T.%e] %L: %v", spdlog::pattern_time_type::local, "");
	s_cpp_logger->set_formatter(std::move(formatter));
	s_cpp_logger->set_level(spdlog::level::trace);
	s_cpp_logger->info("\n");
#endif

	ALERT_LOG_TO("", false);
}

//-------------------------------------------------------------------------------------
void DebugHelper::finalise(bool destroy)
{
	if(!destroy)
	{
		while(DebugHelper::getSingleton().hasBufferedLogPackets() > 0)
		{
			size_t size = DebugHelper::getSingleton().hasBufferedLogPackets();
			Network::Channel* pLoggerChannel = DebugHelper::getSingleton().pLoggerChannel();
			if (pLoggerChannel)
			{
				DebugHelper::getSingleton().sync();
				pLoggerChannel->send();
			}

			if(DebugHelper::getSingleton().hasBufferedLogPackets() == size)
				break;

			sleep(10);
		}

		sleep(1000);
	}

	DebugHelper::getSingleton().clearBufferedLog(true);

	// SAFE_RELEASE(g_pDebugHelperSyncHandler);

#ifndef NO_USE_LOG4CXX
#endif
}

//-------------------------------------------------------------------------------------
Network::Channel* DebugHelper::pLoggerChannel()
{
	if(Network::Address::NONE == loggerAddr_)
		return NULL;

	return pNetworkInterface_->findChannel(loggerAddr_);
}

//-------------------------------------------------------------------------------------
void DebugHelper::clearBufferedLog(bool destroy)
{
	int8 v = Network::g_trace_packet;
	Network::g_trace_packet = 0;

	if(destroy)
	{
		while(!bufferedLogPackets_.empty())
		{
			Network::Bundle* pBundle = bufferedLogPackets_.front();
			bufferedLogPackets_.pop();
			delete pBundle;
		}

		while (!childThreadBufferedLogPackets_.empty())
		{
			MemoryStream* pMemoryStream = childThreadBufferedLogPackets_.front();
			childThreadBufferedLogPackets_.pop();
			delete pMemoryStream;
		}
	}
	else
	{
		Network::Bundle::ObjPool().reclaimObject(bufferedLogPackets_);
		memoryStreamPool_.reclaimObject(childThreadBufferedLogPackets_);
	}

	Network::g_trace_packet = v;

	hasBufferedLogPackets_ = 0;
	noSyncLog_ = true;
	canLogFile_ = true;

	if(!destroy)
		g_pDebugHelperSyncHandler->cancel();
}

//-------------------------------------------------------------------------------------
void DebugHelper::sync()
{
	lockthread();

	if(hasBufferedLogPackets_ == 0)
	{
		unlockthread();
		return;
	}

	// 将子线程日志放入bufferedLogPackets_
	while (childThreadBufferedLogPackets_.size() > 0)
	{
		// 从主对象池取出一个对象，将子线程中对象vector内存交换进去
		MemoryStream* pMemoryStream = childThreadBufferedLogPackets_.front();
		childThreadBufferedLogPackets_.pop();

		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		bufferedLogPackets_.push(pBundle);

		pBundle->newMessage(LoggerInterface::writeLog);
		pBundle->finiCurrPacket();
		pBundle->newPacket();

		// 将他们的内存交换进去
		pBundle->pCurrPacket()->swap(*pMemoryStream);
		pBundle->currMsgLength(pBundle->currMsgLength() + pBundle->pCurrPacket()->length());

		// 将所有对象交还给对象池
		memoryStreamPool_.reclaimObject(pMemoryStream);
	}

	if (Network::Address::NONE == loggerAddr_)
	{
		// 如果超过300秒没有找到logger，那么强制清理内存
		if (timestamp() - loseLoggerTime_ > uint64(300 * stampsPerSecond()))
		{
			clearBufferedLog();
		}
		else
		{
			if (g_kbeSrvConfig.tickMaxBufferedLogs() > 0)
			{
				if (hasBufferedLogPackets_ > g_kbeSrvConfig.tickMaxBufferedLogs())
				{
					clearBufferedLog();
				}
			}
			else
			{
				if (hasBufferedLogPackets_ > 256)
				{
					clearBufferedLog();
				}
			}
		}

		unlockthread();
		return;
	}
	
	Network::Channel* pLoggerChannel = pNetworkInterface_->findChannel(loggerAddr_);
	if(pLoggerChannel == NULL)
	{
		if(g_kbeSrvConfig.tickMaxBufferedLogs() > 0)
		{
			if(hasBufferedLogPackets_ > g_kbeSrvConfig.tickMaxBufferedLogs())
			{
				clearBufferedLog();
			}
		}
		else
		{
			if(hasBufferedLogPackets_ > 256)
			{
				clearBufferedLog();
			}
		}
		
		unlockthread();
		return;
	}

	static bool alertmsg = false;
	if(!alertmsg)
	{
		s_cpp_logger->warn("Forwarding logs to logger[{}]...\n", pLoggerChannel->c_str());
		alertmsg = true;
	}

	int8 v = Network::g_trace_packet;
	Network::g_trace_packet = 0;

	uint32 i = 0;

	Network::Channel::Bundles& bundles = pLoggerChannel->bundles();

	while(!bufferedLogPackets_.empty())
	{
		if((g_kbeSrvConfig.tickMaxSyncLogs() > 0 && i++ >= g_kbeSrvConfig.tickMaxSyncLogs()))
			break;
		
		Network::Bundle* pBundle = bufferedLogPackets_.front();
		bufferedLogPackets_.pop();

		pBundle->finiMessage(true);
		bundles.push_back(pBundle);
		--hasBufferedLogPackets_;
	}

	// 这里需要延时发送，否则在发送过程中产生错误，导致日志输出会出现死锁
	if(bundles.size() > 0 && !pLoggerChannel->sending())
		pLoggerChannel->delayedSend();

	Network::g_trace_packet = v;
	canLogFile_ = false;
	unlockthread();
}

//-------------------------------------------------------------------------------------
void DebugHelper::pDispatcher(Network::EventDispatcher* dispatcher)
{ 
	pDispatcher_ = dispatcher; 
	g_pDebugHelperSyncHandler->startActiveTick();
}

//-------------------------------------------------------------------------------------
void DebugHelper::pNetworkInterface(Network::NetworkInterface* networkInterface)
{ 
	pNetworkInterface_ = networkInterface; 
}

//-------------------------------------------------------------------------------------
void DebugHelper::onMessage(uint32 logType, const char * str, uint32 length)
{
	if (!canLog(logType))
		return;

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

		struct timeb tp;
		ftime(&tp);

		int64 t = tp.time;
		(*pMemoryStream) << t;
		uint32 millitm = tp.millitm;
		(*pMemoryStream) << millitm;
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
			s_cpp_logger->warn("DebugHelper::onMessage: bufferedLogPackets is full({} > kbengine[_defs].xml->logger->tick_max_buffered_logs->{})!\n", 
				hasBufferedLogPackets_, g_kbeSrvConfig.tickMaxBufferedLogs());
#endif

			Network::g_trace_packet = v;

			clearBufferedLog();
			
#ifdef NO_USE_LOG4CXX
#else
			s_cpp_logger->warn("DebugHelper::onMessage: discard logs!\n");
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

		struct timeb tp;
		ftime(&tp);

		int64 t = tp.time;
		(*pBundle) << t;
		uint32 millitm = tp.millitm;
		(*pBundle) << millitm;
		pBundle->appendBlob(str, length);

		bufferedLogPackets_.push(pBundle);
		Network::g_trace_packet = trace_packet;
		g_pDebugHelperSyncHandler->startActiveTick();
	}

	++hasBufferedLogPackets_;
}

//-------------------------------------------------------------------------------------
void DebugHelper::registerLogger(Network::MessageID msgID, Network::Address* pAddr)
{
	loggerAddr_ = *pAddr;
	ALERT_LOG_TO("logger_", true);
}

//-------------------------------------------------------------------------------------
void DebugHelper::unregisterLogger(Network::MessageID msgID, Network::Address* pAddr)
{
	loggerAddr_ = Network::Address::NONE;
	canLogFile_ = true;
	loseLoggerTime_ = timestamp();
	ALERT_LOG_TO("", true);
	printBufferedLogs();
}

//-------------------------------------------------------------------------------------
void DebugHelper::DebugHelper::onNoLogger()
{
}

//-------------------------------------------------------------------------------------
void DebugHelper::printBufferedLogs()
{
	lockthread();

	if(hasBufferedLogPackets_ == 0)
	{
		unlockthread();
		return;
	}

#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->info("The following logs sent to logger failed:\n");
#endif

	// 将子线程日志放入bufferedLogPackets_
	while (childThreadBufferedLogPackets_.size() > 0)
	{
		// 从主对象池取出一个对象，将子线程中对象vector内存交换进去
		MemoryStream* pMemoryStream = childThreadBufferedLogPackets_.front();
		childThreadBufferedLogPackets_.pop();

		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		bufferedLogPackets_.push(pBundle);

		pBundle->newMessage(LoggerInterface::writeLog);
		pBundle->finiCurrPacket();
		pBundle->newPacket();

		// 将他们的内存交换进去
		pBundle->pCurrPacket()->swap(*pMemoryStream);
		pBundle->currMsgLength(pBundle->currMsgLength() + pBundle->pCurrPacket()->length());

		// 将所有对象交还给对象池
		memoryStreamPool_.reclaimObject(pMemoryStream);
	}

	while(!bufferedLogPackets_.empty())
	{		
		Network::Bundle* pBundle = bufferedLogPackets_.front();
		bufferedLogPackets_.pop();

		pBundle->finiMessage(true);

		Network::MessageID msgID;
		Network::MessageLength msglen;
		Network::MessageLength1 msglen1;

		int32 uid;
		uint32 logtype;
		COMPONENT_TYPE componentType;
		COMPONENT_ID componentID;
		COMPONENT_ORDER componentGlobalOrder;
		COMPONENT_ORDER componentGroupOrder;
		int64 t;
		GAME_TIME kbetime;

		std::string str;

		(*pBundle) >> msgID;
		(*pBundle) >> msglen;

		if (msglen == 65535)
			(*pBundle) >> msglen1;

		(*pBundle) >> uid;
		(*pBundle) >> logtype;
		(*pBundle) >> componentType;
		(*pBundle) >> componentID;
		(*pBundle) >> componentGlobalOrder;
		(*pBundle) >> componentGroupOrder;
		(*pBundle) >> t;
		(*pBundle) >> kbetime;
		(*pBundle).readBlob(str);

		time_t tt = static_cast<time_t>(t);	
	    tm* aTm = localtime(&tt);
	    //       YYYY   year
	    //       MM     month (2 digits 01-12)
	    //       DD     day (2 digits 01-31)
	    //       HH     hour (2 digits 00-23)
	    //       MM     minutes (2 digits 00-59)
	    //       SS     seconds (2 digits 00-59)

		if(aTm == NULL)
		{
			Network::Bundle::ObjPool().reclaimObject(pBundle);
			continue;
		}
	
		char timebuf[MAX_BUF];
	    kbe_snprintf(timebuf, MAX_BUF, " [%-4d-%02d-%02d %02d:%02d:%02d %03d] ", aTm->tm_year+1900, aTm->tm_mon+1, 
			aTm->tm_mday, aTm->tm_hour, aTm->tm_min, aTm->tm_sec, kbetime);

		std::string logstr = fmt::format("==>{}", timebuf);
		logstr += str;
		
#ifndef NO_USE_LOG4CXX
		switch (logtype)
		{
		case KBELOG_PRINT:
			s_cpp_logger->info(logstr);
			break;
		case KBELOG_ERROR:
			s_cpp_logger->error(logstr);
			break;
		case KBELOG_WARNING:
			s_cpp_logger->warn(logstr);
			break;
		case KBELOG_DEBUG:
			s_cpp_logger->debug(logstr);
			break;
		case KBELOG_INFO:
			s_cpp_logger->info(logstr);
			break;
		case KBELOG_CRITICAL:
			s_cpp_logger->critical(logstr);
			break;
		case KBELOG_SCRIPT_INFO:
			s_cpp_logger->info(logstr+"[SCRIPT] ");
			break;
		case KBELOG_SCRIPT_ERROR:
			s_cpp_logger->error(logstr + "[SCRIPT] ");
			break;
		case KBELOG_SCRIPT_DEBUG:
			s_cpp_logger->debug(logstr + "[SCRIPT] ");
			break;
		case KBELOG_SCRIPT_WARNING:
			s_cpp_logger->warn(logstr + "[SCRIPT] ");
			break;
		case KBELOG_SCRIPT_NORMAL:
			s_cpp_logger->trace(logstr + "[SCRIPT] ");
			break;
		default:
			break;
		};
#endif

		--hasBufferedLogPackets_;
		Network::Bundle::ObjPool().reclaimObject(pBundle);
	}

	unlockthread();
}

//-------------------------------------------------------------------------------------
void DebugHelper::print_msg(const std::string& s)
{
	KBEngine::thread::ThreadGuard tg(&this->logMutex); 

#ifdef NO_USE_LOG4CXX
#else
	if(canLogFile_)
		s_cpp_logger->info(s);
#endif

	onMessage(KBELOG_PRINT, s.c_str(), (uint32)s.size());
}

//-------------------------------------------------------------------------------------
void DebugHelper::error_msg(const std::string& s)
{
	KBEngine::thread::ThreadGuard tg(&this->logMutex); 

#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->error(s);
#endif

	onMessage(KBELOG_ERROR, s.c_str(), (uint32)s.size());

	set_errorcolor();
	printf("%s%02d: [ERROR]: %s", COMPONENT_NAME_EX_2(g_componentType), g_componentGroupOrder, s.c_str());
	set_normalcolor();
}

//-------------------------------------------------------------------------------------
void DebugHelper::info_msg(const std::string& s)
{
	KBEngine::thread::ThreadGuard tg(&this->logMutex); 

#ifdef NO_USE_LOG4CXX
#else
	if(canLogFile_)
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
	KBEngine::thread::ThreadGuard tg(&this->logMutex); 

#ifdef NO_USE_LOG4CXX
#else
	if(canLogFile_)
		s_cpp_logger->info(s);
#endif

	onMessage(KBELOG_TYPE_MAPPING(scriptMsgType_), s.c_str(), (uint32)s.size());

	// 如果是用户手动设置的也输出为错误信息
	if(spdlog::level::err == scriptMsgType_)
	{
		set_errorcolor();
		printf("%s%02d: [S_ERROR]: %s", COMPONENT_NAME_EX_2(g_componentType), g_componentGroupOrder, s.c_str());
		set_normalcolor();
	}
}

//-------------------------------------------------------------------------------------
void DebugHelper::script_error_msg(const std::string& s)
{
	KBEngine::thread::ThreadGuard tg(&this->logMutex); 

#ifdef NO_USE_LOG4CXX
#else
	if (canLogFile_)
		s_cpp_logger->error(s);
#endif

	onMessage(KBELOG_SCRIPT_ERROR, s.c_str(), (uint32)s.size());

	set_errorcolor();
	printf("%s%02d: [S_ERROR]: %s", COMPONENT_NAME_EX_2(g_componentType), g_componentGroupOrder, s.c_str());
	set_normalcolor();
}

//-------------------------------------------------------------------------------------
void DebugHelper::setScriptMsgType(int msgtype)
{
	scriptMsgType_ = (spdlog::level::level_enum)msgtype;
}

//-------------------------------------------------------------------------------------
void DebugHelper::resetScriptMsgType()
{
	setScriptMsgType(0);
}

//-------------------------------------------------------------------------------------
void DebugHelper::debug_msg(const std::string& s)
{
	KBEngine::thread::ThreadGuard tg(&this->logMutex); 

#ifdef NO_USE_LOG4CXX
#else
	if (canLogFile_)
		s_cpp_logger->debug(s);
#endif

	onMessage(KBELOG_DEBUG, s.c_str(), (uint32)s.size());
}

//-------------------------------------------------------------------------------------
void DebugHelper::warning_msg(const std::string& s)
{
	KBEngine::thread::ThreadGuard tg(&this->logMutex); 

#ifdef NO_USE_LOG4CXX
#else
	if(canLogFile_)
		s_cpp_logger->warn(s);
#endif

	onMessage(KBELOG_WARNING, s.c_str(), (uint32)s.size());

#if KBE_PLATFORM == PLATFORM_WIN32
	set_warningcolor();
	//printf("%s%02d: [WARNING]: %s", COMPONENT_NAME_EX_2(g_componentType), g_componentGroupOrder, s.c_str());
	set_normalcolor();
#endif
}

//-------------------------------------------------------------------------------------
void DebugHelper::critical_msg(const std::string& s)
{
	KBEngine::thread::ThreadGuard tg(&this->logMutex); 

	char buf[DBG_PT_SIZE];
	kbe_snprintf(buf, DBG_PT_SIZE, "%s(%d) -> %s\n\t%s\n", _currFile.c_str(), _currLine, _currFuncName.c_str(), s.c_str());

#ifdef NO_USE_LOG4CXX
#else
	s_cpp_logger->critical(std::string(buf));
#endif

#if KBE_PLATFORM == PLATFORM_WIN32
	set_errorcolor();
	printf("%s%02d: [FATAL]: %s", COMPONENT_NAME_EX_2(g_componentType), g_componentGroupOrder, s.c_str());
	set_normalcolor();
#endif

	onMessage(KBELOG_CRITICAL, buf, (uint32)strlen(buf));
	backtrace_msg();
}

//-------------------------------------------------------------------------------------
void DebugHelper::set_errorcolor()
{
#if KBE_PLATFORM == PLATFORM_WIN32
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_RED);
#endif
}

//-------------------------------------------------------------------------------------
void DebugHelper::set_normalcolor()
{
#if KBE_PLATFORM == PLATFORM_WIN32
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_RED|FOREGROUND_GREEN|
		FOREGROUND_BLUE);
#endif
}

//-------------------------------------------------------------------------------------
void DebugHelper::set_warningcolor()
{
#if KBE_PLATFORM == PLATFORM_WIN32
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_RED|FOREGROUND_GREEN);
#endif
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
			KBE_LOG4CXX_INFO(g_logger, ss);
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


