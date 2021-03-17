// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "network/event_dispatcher.h"
#include "network/event_poller.h"
#include "network/network_interface.h"
#include "py_file_descriptor.h"
#include "server/components.h"
#include "helper/debug_helper.h"
#include "pyscript/pyobject_pointer.h"

namespace KBEngine{

//-------------------------------------------------------------------------------------
PyFileDescriptor::PyFileDescriptor(int fd, sol::function cb, bool write) : 
	fd_(fd),
	cb_(cb),
	write_(write)
{
	if(write)
		Components::getSingleton().pNetworkInterface()->dispatcher().registerWriteFileDescriptor(fd_, this);
	else
		Components::getSingleton().pNetworkInterface()->dispatcher().registerReadFileDescriptor(fd_, this);
}

//-------------------------------------------------------------------------------------
PyFileDescriptor::~PyFileDescriptor()
{
	if(write_)
		Components::getSingleton().pNetworkInterface()->dispatcher().deregisterWriteFileDescriptor(fd_);
	else
		Components::getSingleton().pNetworkInterface()->dispatcher().deregisterReadFileDescriptor(fd_);
}

//-------------------------------------------------------------------------------------
bool PyFileDescriptor::__py_registerReadFileDescriptor(sol::this_state L, int fd, sol::function cb)
{
	if(fd <= 0)
	{
		luaL_error(L, "KBEngine::registerReadFileDescriptor: fd <= 0!");
		return false;
	}

	new PyFileDescriptor(fd, cb, false);
	return true;
}

//-------------------------------------------------------------------------------------
void PyFileDescriptor::__py_deregisterReadFileDescriptor(sol::this_state L, int fd)
{
	if(fd <= 0)
	{
		luaL_error(L, "KBEngine::deregisterReadFileDescriptor: fd <= 0!");
		return;
	}

	PyFileDescriptor* pPyFileDescriptor = 
		static_cast<PyFileDescriptor*>(Components::getSingleton().pNetworkInterface()->dispatcher().pPoller()->findForRead(fd));

	if(pPyFileDescriptor)
		delete pPyFileDescriptor;
}

//-------------------------------------------------------------------------------------
bool PyFileDescriptor::__py_registerWriteFileDescriptor(sol::this_state L, int fd, sol::function cb)
{
	if(fd <= 0)
	{
		luaL_error(L, "KBEngine::registerWriteFileDescriptor: fd <= 0!");
		return false;
	}

	new PyFileDescriptor(fd, cb, true);
	return true;
}

//-------------------------------------------------------------------------------------
void PyFileDescriptor::__py_deregisterWriteFileDescriptor(sol::this_state L, int fd)
{
	
	if(fd <= 0)
	{
		luaL_error(L, "KBEngine::registerWriteFileDescriptor: fd <= 0!");
		return;
	}

	PyFileDescriptor* pPyFileDescriptor = 
		static_cast<PyFileDescriptor*>(Components::getSingleton().pNetworkInterface()->dispatcher().pPoller()->findForWrite(fd));

	if(pPyFileDescriptor)
		delete pPyFileDescriptor;
}

//-------------------------------------------------------------------------------------
int PyFileDescriptor::handleInputNotification(int fd)
{
	//INFO_MSG(fmt::format("PyFileDescriptor:handleInputNotification: fd = {}\n",
	//			fd));

	callback();
	return 0;
}

//-------------------------------------------------------------------------------------
int PyFileDescriptor::handleOutputNotification( int fd )
{
	//INFO_MSG(fmt::format("PyFileDescriptor:handleOutputNotification: fd = {}\n",
	//			fd));

	callback();
	return 0;
}

//-------------------------------------------------------------------------------------
void PyFileDescriptor::callback()
{
	cb_(fd_);
}

//-------------------------------------------------------------------------------------
}
