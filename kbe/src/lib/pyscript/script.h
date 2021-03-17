// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBENGINE_SCRIPT_H
#define KBENGINE_SCRIPT_H

#include "helper/debug_helper.h"
#include "common/common.h"
#include "common/singleton.h"
#include "scriptobject.h"
#include "scriptstdouterr.h"
#include "scriptstdouterrhook.h"

namespace KBEngine{ namespace script{

/** �ű�ϵͳ·�� */
#ifdef _LP64
#define SCRIPT_PATH													\
					L"../../res/scripts;"							\
					L"../../res/scripts/common;"					\
					L"../../res/scripts/common/lib-dynload;"		\
					L"../../res/scripts/common/DLLs;"				\
					L"../../res/scripts/common/Lib;"				\
					L"../../res/scripts/common/Lib/site-packages;"	\
					L"../../res/scripts/common/Lib/dist-packages"

#else
#define SCRIPT_PATH													\
					L"../../res/scripts;"							\
					L"../../res/scripts/common;"					\
					L"../../res/scripts/common/lib-dynload;"		\
					L"../../res/scripts/common/DLLs;"				\
					L"../../res/scripts/common/Lib;"				\
					L"../../res/scripts/common/Lib/site-packages;"	\
					L"../../res/scripts/common/Lib/dist-packages"

#endif

#define APPEND_PYSYSPATH(PY_PATHS)									\
	std::wstring pySysPaths = SCRIPT_PATH;							\
	wchar_t* pwpySysResPath = strutil::char2wchar(const_cast<char*>(Resmgr::getSingleton().getPySysResPath().c_str()));	\
	strutil::kbe_replace(pySysPaths, L"../../res/", pwpySysResPath);\
	PY_PATHS += pySysPaths;											\
	free(pwpySysResPath);

class Script: public Singleton<Script>
{						
public:	
	Script();
	virtual ~Script();
	
	/** 
		��װ��ж�ؽű�ģ�� 
	*/
	virtual bool install(const wchar_t* pythonHomeDir, std::wstring pyPaths, 
		const char* moduleName, COMPONENT_TYPE componentType);
	virtual bool uninstall(void);

	bool InitLua(const char* home, const char* moduleName, COMPONENT_TYPE componentType, const char* paths, const char* cpaths);
	bool UnintiLua();

	bool DoFile(const char* filename);

	bool installExtraModule(const char* moduleName);

	/** 
		���һ����չ�ӿڵ�������չģ�� 
	*/
	bool registerExtraMethod(const char* attrName, PyMethodDef* pyFunc);

	/** 
		���һ����չ���Ե�������չģ�� 
	*/
	bool registerExtraObject(const char* attrName, PyObject* pyObj);

	/** 
		��ȡ�ű�����ģ�� 
	*/
	INLINE PyObject* getModule(void) const;
	INLINE sol::main_table getLuaModule(void) const;

	/** 
		��ȡ�ű���չģ�� 
	*/
	INLINE PyObject* getExtraModule(void) const;

	/**
		��ȡ�ű���ʼ��ʱ����ģ��
	*/
	INLINE PyObject* getSysInitModules(void) const;

	int run_simpleString(const char* command, std::string* retBufferPtr);
	INLINE int run_simpleString(std::string command, std::string* retBufferPtr);

	int registerToModule(const char* attrName, PyObject* pyObj);
	int unregisterToModule(const char* attrName);

	INLINE ScriptStdOutErr* pyStdouterr() const;

	INLINE void pyPrint(const std::string& str);

	void setenv(const std::string& name, const std::string& value);

private:
	void _addLuaPaths(const char* path, const char* cpath);

protected:
	PyObject* 					module_;
	PyObject*					extraModule_;		// ��չ�ű�ģ��
	PyObject*					sysInitModules_;	// ��ʼʱsys���ص�ģ��

	ScriptStdOutErr*			pyStdouterr_;

	std::string					script_home_;
	sol::main_table				lua_module_;
	sol::state					lua_;
} ;

}
}

#ifdef CODE_INLINE
#include "script.inl"
#endif

#endif // KBENGINE_SCRIPT_H
