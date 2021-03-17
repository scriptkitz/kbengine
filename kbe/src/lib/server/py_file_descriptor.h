// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_PY_FILE_DESCRIPTOR_H
#define KBE_PY_FILE_DESCRIPTOR_H

#include "common/common.h"
#include "pyscript/scriptobject.h"
#include "common/smartpointer.h"

namespace KBEngine{
typedef SmartPointer<PyObject> PyObjectPtr;

class PyFileDescriptor : public Network::InputNotificationHandler, public Network::OutputNotificationHandler
{
public:
	PyFileDescriptor(int fd, sol::function cb, bool write);
	virtual ~PyFileDescriptor();
	
	/** 
		脚本请求(注册/注销)文件描述符(读和写)
	*/
	static bool __py_registerReadFileDescriptor(sol::this_state L, int fd, sol::function cb);
	static bool __py_registerWriteFileDescriptor(sol::this_state L, int fd, sol::function cb);
	static void __py_deregisterReadFileDescriptor(sol::this_state L, int fd);
	static void __py_deregisterWriteFileDescriptor(sol::this_state L, int fd);
protected:

	virtual int handleInputNotification( int fd );
	virtual int handleOutputNotification( int fd );

	void callback();

	int fd_;
	sol::function cb_;

	bool write_;
};

}

#endif // KBE_PY_FILE_DESCRIPTOR_H
