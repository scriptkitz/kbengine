// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "python_app.h"
#include "pyscript/py_memorystream.h"
#include "server/py_file_descriptor.h"

namespace KBEngine{

KBEngine::ScriptTimers KBEngine::PythonApp::scriptTimers_;

/**
内部定时器处理类
*/
class ScriptTimerHandler : public TimerHandler
{
public:
	ScriptTimerHandler(ScriptTimers* scriptTimers, sol::function callback) :
		cb_(callback),
		scriptTimers_(scriptTimers)
	{
	}

	~ScriptTimerHandler()
	{
	}

private:
	virtual void handleTimeout(TimerHandle handle, void * pUser)
	{
		int id = ScriptTimersUtil::getIDForHandle(scriptTimers_, handle);
		cb_(id);
		return;
	}

	virtual void onRelease(TimerHandle handle, void * /*pUser*/)
	{
		scriptTimers_->releaseTimer(handle);
		delete this;
	}

	sol::function cb_;
	ScriptTimers* scriptTimers_;
};

//-------------------------------------------------------------------------------------
PythonApp::PythonApp(Network::EventDispatcher& dispatcher, 
					 Network::NetworkInterface& ninterface, 
					 COMPONENT_TYPE componentType,
					 COMPONENT_ID componentID):
ServerApp(dispatcher, ninterface, componentType, componentID),
script_(),
entryScript_()
{
	ScriptTimers::initialize(*this);
}

//-------------------------------------------------------------------------------------
PythonApp::~PythonApp()
{
}

//-------------------------------------------------------------------------------------
bool PythonApp::inInitialize()
{
	if(!installLuaScript())
		return false;
	if (!installLuaModules())
		return false;

	return true;
}

//-------------------------------------------------------------------------------------	
bool PythonApp::initializeEnd()
{
	gameTickTimerHandle_ = this->dispatcher().addTimer(1000000 / g_kbeSrvConfig.gameUpdateHertz(), this,
		reinterpret_cast<void *>(TIMEOUT_GAME_TICK));
	
	return true;
}

//-------------------------------------------------------------------------------------	
void PythonApp::onShutdownBegin()
{
	ServerApp::onShutdownBegin();
}

//-------------------------------------------------------------------------------------	
void PythonApp::onShutdownEnd()
{
	ServerApp::onShutdownEnd();
}

//-------------------------------------------------------------------------------------
void PythonApp::finalise(void)
{
	gameTickTimerHandle_.cancel();
	scriptTimers_.cancelAll();
	ScriptTimers::finalise(*this);

	uninstallPyScript();
	ServerApp::finalise();
}

//-------------------------------------------------------------------------------------
void PythonApp::handleTimeout(TimerHandle handle, void * arg)
{
	ServerApp::handleTimeout(handle, arg);

	switch (reinterpret_cast<uintptr>(arg))
	{
	case TIMEOUT_GAME_TICK:
		++g_kbetime;
		handleTimers();
		break;
	default:
		break;
	}
}

//-------------------------------------------------------------------------------------
int PythonApp::registerPyObjectToScript(const char* attrName, PyObject* pyObj)
{ 
	return script_.registerToModule(attrName, pyObj); 
}

//-------------------------------------------------------------------------------------
int PythonApp::unregisterPyObjectToScript(const char* attrName)
{ 
	return script_.unregisterToModule(attrName); 
}

//-------------------------------------------------------------------------------------
bool PythonApp::installLuaScript()
{
	if (Resmgr::getSingleton().respaths().size() <= 0 ||
		Resmgr::getSingleton().getPyUserResPath().size() == 0 ||
		Resmgr::getSingleton().getPySysResPath().size() == 0 ||
		Resmgr::getSingleton().getPyUserScriptsPath().size() == 0)
	{
		KBE_ASSERT(false && "PythonApp::installLuaScript: KBE_RES_PATH error!\n");
		return false;
	}

	std::string user_scripts_path = Resmgr::getSingleton().getPyUserScriptsPath();

	std::string paths = user_scripts_path + "common/?.lua;";
	std::string homedir;

	switch (componentType_)
	{
	case BASEAPP_TYPE:
		homedir = user_scripts_path + "base/";
		break;
	case CELLAPP_TYPE:
		homedir = user_scripts_path + "cell/";
		break;
	case DBMGR_TYPE:
		homedir = user_scripts_path + "db/";
		break;
	case INTERFACES_TYPE:
		homedir = user_scripts_path + "interface/";
		break;
	case LOGINAPP_TYPE:
		homedir = user_scripts_path + "login/";
		break;
	default:
		homedir = user_scripts_path + "client/";
		break;
	};

	paths += user_scripts_path + "server_common/?.lua;";
	paths += homedir + "?.lua;";

	getScript().InitLua(homedir.c_str(), "KBEngine", componentType_, paths.c_str(), nullptr);
	
	return true;
}

//-------------------------------------------------------------------------------------
bool PythonApp::uninstallPyScript()
{
	script::PyMemoryStream::uninstallScript();
	return uninstallPyModules() && getScript().uninstall();
}

//-------------------------------------------------------------------------------------
bool PythonApp::uninstallLuaScript()
{
	script::PyMemoryStream::uninstallScript();
	return uninstallLuaModules() && getScript().UnintiLua();
}

//-------------------------------------------------------------------------------------
bool PythonApp::installLuaModules()
{
	// 安装入口模块
	std::string scriptfilename = "";

	if (componentType() == BASEAPP_TYPE)
	{
		scriptfilename = g_kbeSrvConfig.getBaseApp().entryScriptFile;
	}
	else if (componentType() == CELLAPP_TYPE)
	{
		scriptfilename = g_kbeSrvConfig.getCellApp().entryScriptFile;
	}
	else if (componentType() == INTERFACES_TYPE)
	{
		scriptfilename = g_kbeSrvConfig.getInterfaces().entryScriptFile;
	}
	else if (componentType() == LOGINAPP_TYPE)
	{
		scriptfilename = g_kbeSrvConfig.getLoginApp().entryScriptFile;
	}
	else if (componentType() == DBMGR_TYPE)
	{
		scriptfilename = g_kbeSrvConfig.getDBMgr().entryScriptFile;
	}
	else
	{
		ERROR_MSG("PythonApp::installPyModules: Unsupported script!\n");
	}

	sol::main_table module = getScript().getLuaModule();
	module.set_function("MemoryStream", script::PyMemoryStream::py_new);

	// 注册创建entity的方法到py
	// 获取apps发布状态, 可在脚本中获取该值
	module.set_function("publish", []()->int8 { return g_appPublish; });

	// 注册设置脚本输出类型
	module.set_function("scriptLogType", [](int level) { DebugHelper::getSingleton().setScriptMsgType(level); });

	// 获得资源全路径
	module.set_function("getResFullPath", [](const char* res)-> const char* {
		if (!Resmgr::getSingleton().hasRes(res)) return "";
		return Resmgr::getSingleton().matchRes(res).c_str();
	});

	// 是否存在某个资源
	module.set_function("hasRes", [](const char* res)-> bool { return Resmgr::getSingleton().hasRes(res); });

	// 打开一个文件
	module.set_function("open", [](sol::this_state s, const char* file, const char* mode) {
		std::string sfullpath = Resmgr::getSingleton().matchRes(file);
		sol::userdata ud;
		const char* msg = nullptr;

		lua_State* L = s;
		lua_getglobal(L, "io");
		lua_getfield(L, -1, "open");
		lua_pushstring(L, sfullpath.c_str());
		lua_pushstring(L, mode);
		lua_call(L, 2, 2);
		if (lua_isuserdata(L, -2))
		{
			ud = sol::userdata(L, -2);
		}
		else
		{
			msg = lua_tostring(L, -1);
		}
		lua_pop(L, 3);
		return std::make_tuple(ud, msg);
	});
	
	/*
	extfilter: "lua" / "lua|txt|exe"
	*/

	std::function _listPathResFunc = [](sol::this_state s, const char* respath, const char* extfilter) {
		lua_State* L = s;
		std::wstring wextfilter;
		std::vector<std::wstring> results;
		sol::table tb = sol::table::create(L);

		if (!respath) return tb;

		std::string foundPath = Resmgr::getSingleton().matchPath(respath);
		if (foundPath.size() == 0) return tb;
		wchar_t* twc = strutil::char2wchar(foundPath.c_str());
		std::wstring wrespath(twc); free(twc);
		if (extfilter)
		{
			twc = strutil::char2wchar(extfilter);
			wextfilter = twc[0] == L'.' ? twc+1 : twc; free(twc);
		}
		else
		{
			wextfilter = L"*.*";
		}
		Resmgr::getSingleton().listPathRes(wrespath, wextfilter, results);
		for each (std::wstring s in results)
		{
			char* tws = strutil::wchar2char(s.c_str());
			tb.add(tws); free(tws);
		}

		return tb;
	};

	// 列出目录下所有文件
	module.set_function("listPathRes", sol::overload(
		[_listPathResFunc](sol::this_state L, const char* respath) { return _listPathResFunc(L, respath, nullptr); },
		_listPathResFunc));

	// 匹配相对路径获得全路径
	module.set_function("matchPath", [](const char* path) {
		return Resmgr::getSingleton().matchPath(path);
	});

	// debug追踪kbe封装的py对象计数
	module.set_function("debugTracing", []() { script::PyGC::debugTracing(false); });


	module.set("LOG_TYPE_TRACE", SPDLOG_LEVEL_TRACE);
	module.set("LOG_TYPE_DBG", SPDLOG_LEVEL_DEBUG);
	module.set("LOG_TYPE_INFO", SPDLOG_LEVEL_INFO);
	module.set("LOG_TYPE_WAR", SPDLOG_LEVEL_WARN);
	module.set("LOG_TYPE_ERR", SPDLOG_LEVEL_ERROR);
	module.set("LOG_TYPE_CRITICAL", SPDLOG_LEVEL_CRITICAL);
	module.set("NEXT_ONLY", KBE_NEXT_ONLY);


	// 注册所有pythonApp都要用到的通用接口
	module.set_function("addTimer", [](sol::this_state L, float interval, float repeat, sol::function callback) {
		ScriptTimers* pTimers = &scriptTimers();
		ScriptTimerHandler* handler = new ScriptTimerHandler(pTimers, callback);

		ScriptID id = ScriptTimersUtil::addTimer(&pTimers, interval, repeat, 0, handler);

		if (id == 0)
		{
			luaL_error(L, "Unable to add timer");
			return -1;
		}
		return id;
	});
	module.set_function("delTimer", [](sol::this_state L, ScriptID timerID) {

		if (!ScriptTimersUtil::delTimer(&scriptTimers(), timerID))
		{
			luaL_error(L, "delTimer error!");
			return -1;
		}
		return timerID;
	});

	module.set_function("registerReadFileDescriptor", PyFileDescriptor::__py_registerReadFileDescriptor);
	module.set_function("registerWriteFileDescriptor", PyFileDescriptor::__py_registerWriteFileDescriptor);
	module.set_function("deregisterReadFileDescriptor", PyFileDescriptor::__py_deregisterReadFileDescriptor);
	module.set_function("deregisterWriteFileDescriptor", PyFileDescriptor::__py_deregisterWriteFileDescriptor);

	onInstallLuaModules();

	if (!scriptfilename.empty())
	{
		if (!getScript().DoFile((scriptfilename+".lua").c_str()))
			return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool PythonApp::uninstallLuaModules()
{
	return true;
}

//-------------------------------------------------------------------------------------
bool PythonApp::uninstallPyModules()
{
	// script::PyGC::set_debug(script::PyGC::DEBUG_STATS|script::PyGC::DEBUG_LEAK);
	// script::PyGC::collect();

	script::PyGC::debugTracing();
	return true;
}


//-------------------------------------------------------------------------------------
void PythonApp::startProfile_(Network::Channel* pChannel, std::string profileName, 
	int8 profileType, uint32 timelen)
{
	if(pChannel->isExternal())
		return;
	
	switch(profileType)
	{
	case 0:	// pyprofile
		new PyProfileHandler(this->networkInterface(), timelen, profileName, pChannel->addr());
		return;
	default:
		break;
	};

	ServerApp::startProfile_(pChannel, profileName, profileType, timelen);
}

//-------------------------------------------------------------------------------------
void PythonApp::onExecScriptCommand(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if(pChannel->isExternal())
		return;
	
	std::string cmd;
	s.readBlob(cmd);

	PyObject* pycmd = PyUnicode_DecodeUTF8(cmd.data(), cmd.size(), NULL);
	if(pycmd == NULL)
	{
		SCRIPT_ERROR_CHECK();
		return;
	}

	DEBUG_MSG(fmt::format("PythonApp::onExecScriptCommand: size({}), command={}.\n", 
		cmd.size(), cmd));

	std::string retbuf = "";
	PyObject* pycmd1 = PyUnicode_AsEncodedString(pycmd, "utf-8", NULL);
	script_.run_simpleString(PyBytes_AsString(pycmd1), &retbuf);

	if(retbuf.size() == 0)
	{
		retbuf = "\r\n";
	}

	// 将结果返回给客户端
	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	ConsoleInterface::ConsoleExecCommandCBMessageHandler msgHandler;
	(*pBundle).newMessage(msgHandler);
	ConsoleInterface::ConsoleExecCommandCBMessageHandlerArgs1::staticAddToBundle((*pBundle), retbuf);
	pChannel->send(pBundle);

	Py_DECREF(pycmd);
	Py_DECREF(pycmd1);
}

//-------------------------------------------------------------------------------------
void PythonApp::onReloadScript(bool fullReload)
{
}

//-------------------------------------------------------------------------------------
void PythonApp::reloadScript(bool fullReload)
{
	onReloadScript(fullReload);

	// SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	// 所有脚本都加载完毕
	auto onEntryFun = getScript().getLua()["onInit"];
	if (onEntryFun.valid())
	{
		onEntryFun.call(1);
	}
	else
	{
		luaL_error(getScript().getLua().lua_state(), "no onInit!");
	}
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_addTimer(PyObject* self, PyObject* args)
{
	return PyLong_FromLong(0);
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_delTimer(PyObject* self, PyObject* args)
{
	return PyLong_FromLong(0);
}

//-------------------------------------------------------------------------------------
}
