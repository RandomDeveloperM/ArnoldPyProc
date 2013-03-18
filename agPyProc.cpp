#include <Python.h>
#include <ai.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>

// ---

struct ProcData
{
  std::string procName;
  std::string script;
  PyObject *module;
  PyObject *userPtr;
  bool verbose;
};

// ---

class PythonInterpreter
{
public:
  
  struct ThreadData
  {
    PyInterpreterState *istate;
    ProcData *pdata;
    int i;
    AtNode *node;
    int rv;
    PyThreadState *ostate;
    PyThreadState *tstate;
    
    
    ThreadData(ProcData *_pdata,
               int _i)
      : pdata(_pdata)
      , i(_i)
      , node(NULL)
      , rv(-1)
      , ostate(NULL)
      , tstate(NULL)
    {
      PyGILState_STATE gil = PyGILState_Ensure();
      PyThreadState *tstate = PyGILState_GetThisThreadState();
      istate = tstate->interp;
      PyGILState_Release(gil);
    }
    
    void acquire()
    {
      if (istate)
      {
        tstate = PyThreadState_New(istate);
        PyEval_AcquireLock();
        ostate = PyThreadState_Swap(tstate);
      }
    }
    
    void release()
    {
      if (tstate)
      {
        PyThreadState_Swap(ostate);
        PyEval_ReleaseLock();
        PyThreadState_Clear(tstate);
        PyThreadState_Delete(tstate);
        tstate = NULL;
        ostate = NULL;
      }
    }
  };
  
  static unsigned int InitFunc(void *threadData)
  {
    ThreadData *tdata = (ThreadData*) threadData;
    ProcData *data = tdata->pdata;
    
    tdata->acquire();
    
    // Derive python module name
    std::string modname = "pythonDso_";
    size_t p0 = data->script.find_last_of("\\/");
    if (p0 != std::string::npos)
    {
      modname += data->script.substr(p0+1);
    }
    else
    {
      modname += data->script;
    }
    p0 = modname.find('.');
    if (p0 != std::string::npos)
    {
      modname = modname.substr(0, p0);
    }
    
    if (data->verbose)
    {
      AiMsgInfo("[pythonDso] Resolved script path \"%s\"", data->script.c_str());
    }
    
    PyObject *pyimp = PyImport_ImportModule("imp");
    if (pyimp == NULL)
    {
      AiMsgError("[pythonDso] Could not import imp module");
      PyErr_Print();
      PyErr_Clear();
      tdata->rv = 0;
      tdata->release();
      return 0;
    }
    
    PyObject *pyload = PyObject_GetAttrString(pyimp, "load_source");
    if (pyload == NULL)
    {
      AiMsgError("[pythonDso] No \"load_source\" function in imp module");
      PyErr_Print();
      PyErr_Clear();
      Py_DECREF(pyimp);
      tdata->rv = 0;
      tdata->release();
      return 0;
    }
    
    if (data->verbose)
    {
      AiMsgInfo("[pythonDso] Loading procedural module");
    }
    
    data->module = PyObject_CallFunction(pyload, (char*)"ss", modname.c_str(), data->script.c_str());
    
    if (data->module == NULL)
    {
      AiMsgError("[pythonDso] Failed to import procedural python module");
      PyErr_Print();
      PyErr_Clear();
      Py_DECREF(pyload);
      Py_DECREF(pyimp);
      tdata->rv = 0;
      tdata->release();
      return 0;
    }
    
    PyObject *func = PyObject_GetAttrString(data->module, "Init");
    
    if (func)
    {
      PyObject *rv = PyObject_CallFunction(func, (char*)"s", data->procName.c_str());
      
      if (rv)
      {
        if (PyTuple_Check(rv) && PyTuple_Size(rv) == 2)
        {
          data->userPtr = PyTuple_GetItem(rv, 1);
          Py_INCREF(data->userPtr);
          
          tdata->rv = PyInt_AsLong(PyTuple_GetItem(rv, 0));
          if (tdata->rv == -1 && PyErr_Occurred() != NULL)
          {
            AiMsgError("[pythonDso] Invalid return value for \"Init\" function in module \"%s\"", data->script.c_str());
            PyErr_Print();
            PyErr_Clear();
            tdata->rv = 0;
          }
        }
        else
        {
          AiMsgError("[pythonDso] Invalid return value for \"Init\" function in module \"%s\"", data->script.c_str());
          tdata->rv = 0;
        }
        
        Py_DECREF(rv);
      }
      else
      {
        AiMsgError("[pythonDso] \"Init\" function failed in module \"%s\"", data->script.c_str());
        PyErr_Print();
        PyErr_Clear();
        tdata->rv = 0;
      }
      
      Py_DECREF(func);
    }
    else
    {
      AiMsgError("[pythonDso] No \"Init\" function in module \"%s\"", data->script.c_str());
      tdata->rv = 0;
    }
    
    Py_DECREF(pyload);
    Py_DECREF(pyimp);
    
    tdata->release();
    
    return 0;
  }
  
  static unsigned int CleanupFunc(void *threadData)
  {
    ThreadData *tdata = (ThreadData*) threadData;
    ProcData *data = tdata->pdata;
    
    tdata->acquire();
    
    PyObject *func = PyObject_GetAttrString(data->module, "Cleanup");
    
    if (func)
    {
      PyObject *rv = PyObject_CallFunction(func, (char*)"O", data->userPtr);
      
      if (rv)
      {
        tdata->rv = PyInt_AsLong(rv);
        if (tdata->rv == -1 && PyErr_Occurred() != NULL)
        {
          AiMsgError("[pythonDso] Invalid return value for \"Cleanup\" function in module \"%s\"", data->script.c_str());
          PyErr_Print();
          PyErr_Clear();
          tdata->rv = 0;
        }
        Py_DECREF(rv);
      }
      else
      {
        AiMsgError("[pythonDso] \"Cleanup\" function failed in module \"%s\"", data->script.c_str());
        PyErr_Print();
        PyErr_Clear();
        tdata->rv = 0;
      }
      
      Py_DECREF(func);
    }
    else
    {
      AiMsgError("[pythonDso] No \"Cleanup\" function in module \"%s\"", data->script.c_str());
      tdata->rv = 0;
    }
    
    Py_DECREF(data->userPtr);
    Py_DECREF(data->module);
    
    tdata->release();
    
    return 0;
  }
  
  static unsigned int NumNodesFunc(void *threadData)
  {
    ThreadData *tdata = (ThreadData*) threadData;
    ProcData *data = tdata->pdata;
    
    tdata->acquire();
    
    PyObject *func = PyObject_GetAttrString(data->module, "NumNodes");
    
    if (func)
    {
      PyObject *rv = PyObject_CallFunction(func, (char*)"O", data->userPtr);
      
      if (rv)
      {
        tdata->rv = PyInt_AsLong(rv);
        if (tdata->rv == -1 && PyErr_Occurred() != NULL)
        {
          AiMsgError("[pythonDso] Invalid return value for \"NumNodes\" function in module \"%s\"", data->script.c_str());
          PyErr_Print();
          PyErr_Clear();
          tdata->rv = 0;
        }
        Py_DECREF(rv);
      }
      else
      {
        AiMsgError("[pythonDso] \"NumNodes\" function failed in module \"%s\"", data->script.c_str());
        PyErr_Print();
        PyErr_Clear();
        tdata->rv = 0;
      }
      
      Py_DECREF(func);
    }
    else
    {
      AiMsgError("[pythonDso] No \"NumNodes\" function in module \"%s\"", data->script.c_str());
      tdata->rv = 0;
    }
    
    tdata->release();
    
    return 0;
  }
  
  static unsigned int GetNodeFunc(void *threadData)
  {
    ThreadData *tdata = (ThreadData*) threadData;
    ProcData *data = tdata->pdata;
    
    tdata->acquire();
    
    PyObject *func = PyObject_GetAttrString(data->module, "GetNode");
    
    if (func)
    {
      PyObject *rv = PyObject_CallFunction(func, (char*)"Oi", data->userPtr, tdata->i);
      
      if (rv)
      {
        if (!PyString_Check(rv))
        {
          AiMsgError("[pythonDso] Invalid return value for \"GetNode\" function in module \"%s\"", data->script.c_str());
        }
        
        const char *nodeName = PyString_AsString(rv);
        tdata->node = AiNodeLookUpByName(nodeName);
        if (tdata->node == NULL)
        {
          AiMsgError("[pythonDso] Invalid node name \"%s\" return by \"GetNode\" function in modulde \"%s\"", nodeName, data->script.c_str());
        }
        
        Py_DECREF(rv);
      }
      else
      {
        AiMsgError("[pythonDso] \"GetNode\" function failed in module \"%s\"", data->script.c_str());
        PyErr_Print();
        PyErr_Clear();
        tdata->node = 0;
      }
      
      Py_DECREF(func);
    }
    else
    {
      AiMsgError("[pythonDso] No \"GetNode\" function in module \"%s\"", data->script.c_str());
      tdata->node = 0;
    }
    
    tdata->release();
    
    return 0;
  }
  
  static PythonInterpreter& Get()
  {
    if (!msInstance)
    {
      new PythonInterpreter();
    }
    return *msInstance;
  }
  
  static void Finish()
  {
    if (msInstance)
    {
      delete msInstance;
    }
  }
  
private:
  
  PythonInterpreter()
    : mInitialized(false)
    , mRunning(false)
  {
    bool initialized = Py_IsInitialized();
  
    if (initialized)
    {
      AiMsgInfo("[pythonDso] Python already initialized");
      mInitialized = false;
    }
    else
    {
      AiMsgInfo("[pythonDso] Initializing python");
      Py_SetProgramName((char*)"pythonDso");
      Py_Initialize();
      mInitialized = true; 
    }
    
    if (PyEval_ThreadsInitialized() != 0)
    {
      AiMsgInfo("[pythonDso] Re-initialize python threads");
      PyEval_ReInitThreads();
    }
    else
    {
      AiMsgInfo("[pythonDso] Initialize python threads");
      PyEval_InitThreads();
    }
    
    mMainState = PyThreadState_Swap(NULL);
    
    PyEval_ReleaseLock();
    
    mRunning = true;
    
    msInstance = this;
  }
  
  ~PythonInterpreter()
  {
    if (mRunning)
    {
      if (mInitialized)
      {
        AiMsgInfo("[pythonDso] Finalize python");
        PyEval_AcquireLock();
        PyThreadState_Swap(mMainState);
        Py_Finalize();
      }
      mRunning = false;
    }
    
    msInstance = NULL;
  }
  
public:
  
  bool wasInitialized() const
  {
    return !mInitialized;
  }
  
  bool isRunning() const
  {
    return mRunning;
  }
  
  int procInit(ProcData *pdata)
  {
    ThreadData data(pdata, -1);
    
    void *thr = AiThreadCreate(InitFunc, &data, AI_PRIORITY_HIGH);
    AiThreadWait(thr);
    AiThreadClose(thr);
    
    return data.rv;
  }
  
  int procCleanup(ProcData *pdata)
  {
    ThreadData data(pdata, -1);
    
    void *thr = AiThreadCreate(CleanupFunc, &data, AI_PRIORITY_HIGH);
    AiThreadWait(thr);
    AiThreadClose(thr);
    
    return data.rv;
  }
  
  int procNumNodes(ProcData *pdata)
  {
    ThreadData data(pdata, -1);
    
    void *thr = AiThreadCreate(NumNodesFunc, &data, AI_PRIORITY_HIGH);
    AiThreadWait(thr);
    AiThreadClose(thr);
    
    return data.rv;
  }
  
  AtNode* procGetNode(ProcData *pdata, int i)
  {
    ThreadData data(pdata, i);
    
    void *thr = AiThreadCreate(GetNodeFunc, &data, AI_PRIORITY_HIGH);
    AiThreadWait(thr);
    AiThreadClose(thr);
    
    return data.node;
  }
  
private:
  
  bool mInitialized;
  bool mRunning;
  PyThreadState *mMainState;
  static PythonInterpreter *msInstance;
};

PythonInterpreter* PythonInterpreter::msInstance = NULL;

// ---

bool FindInPath(const std::string &procpath, const std::string &script, std::string &path)
{
#ifdef _WIN32
  char sep = ';';
#else
  char sep = ':';
#endif
  
  struct stat st;
  bool found = false;

  size_t p0 = 0;
  size_t p1 = procpath.find(sep, p0);

  while (!found && p1 != std::string::npos)
  {
    path = procpath.substr(p0, p1-p0);
    
    if (path.length() >= 0)
    {
      if (path[0] == '[' && path[path.length()-1] == ']')
      {
        char *env = getenv(path.substr(1, path.length()-2).c_str());
        if (env) found = FindInPath(env, script, path);
      }
      else
      {
        path += "/" + script;
        found = ((stat(path.c_str(), &st) == 0) && ((st.st_mode & S_IFREG) != 0));
      }
    }
    
    p0 = p1 + 1;
    p1 = procpath.find(sep, p0);
  }
  
  if (!found)
  {
    path = procpath.substr(p0);
    
    if (path.length() > 0)
    {
      if (path[0] == '[' && path[path.length()-1] == ']')
      {
        char *env = getenv(path.substr(1, path.length()-2).c_str());
        if (env) found = FindInPath(env, script, path);
      }
      else
      {
        path += "/" + script;
        found = ((stat(path.c_str(), &st) == 0) && ((st.st_mode & S_IFREG) != 0));
      }
    }
  }
  
  return found;
}

// ---

int PyDSOInit(AtNode *node, void **user_ptr)
{
  PythonInterpreter &py = PythonInterpreter::Get();
  if (!py.isRunning())
  {
    return 0;
  }
  
  bool verbose = false;
  if (AiNodeLookUpUserParameter(node, "verbose") != NULL)
  {
    verbose = AiNodeGetBool(node, "verbose");
  }
  
  ProcData *data = new ProcData();
  data->procName = AiNodeGetStr(node, "name");
  data->script = "";
  data->module = NULL;
  data->userPtr = NULL;
  data->verbose = verbose;
  
  *user_ptr = (void*) data;
  
  std::string script = AiNodeGetStr(node, "data");
  
  struct stat st;
  
  if ((stat(script.c_str(), &st) != 0) || ((st.st_mode & S_IFREG) == 0))
  {
    if (verbose)
    {
      AiMsgInfo("[pythonDso] Search python procedural in options.procedural_searchpath...");
    }
    // look in procedural search path
    AtNode *opts = AiUniverseGetOptions();
    if (!opts)
    {
      return 0;
    }
    std::string procpath = AiNodeGetStr(opts, "procedural_searchpath");
    if (!FindInPath(procpath, script, data->script))
    {
      return 0;
    }
  }
  else
  {
    data->script = script;
  }
  
  size_t p0, p1;
  
#ifdef _WIN32
  char dirSepFrom = '/';
  char dirSepTo = '\\';
#else
  char dirSepFrom = '\\';
  char dirSepTo = '/';
#endif
  p0 = 0;
  p1 = data->script.find(dirSepFrom, p0);
  while (p1 != std::string::npos)
  {
    data->script[p1] = dirSepTo;
    p0 = p1 + 1;
    p1 = data->script.find(dirSepFrom, p0);
  }
  
  return py.procInit(data);
}

int PyDSOCleanup(void *user_ptr)
{
  PythonInterpreter &py = PythonInterpreter::Get();
  if (!py.isRunning())
  {
    return 0;
  }
  
  ProcData *data = (ProcData*) user_ptr;
  
  int rv = py.procCleanup(data);
  
  delete data;
  
  return rv;
}

int PyDSONumNodes(void *user_ptr)
{
  PythonInterpreter &py = PythonInterpreter::Get();
  if (!py.isRunning())
  {
    return 0;
  }
  
  ProcData *data = (ProcData*) user_ptr;
  
  return py.procNumNodes(data);
}

AtNode* PyDSOGetNode(void *user_ptr, int i)
{
  PythonInterpreter &py = PythonInterpreter::Get();
  if (!py.isRunning())
  {
    return 0;
  }
  
  ProcData *data = (ProcData*) user_ptr;
  
  return py.procGetNode(data, i);
}

proc_loader
{
  vtable->Init = PyDSOInit;
  vtable->Cleanup = PyDSOCleanup;
  vtable->NumNodes = PyDSONumNodes;
  vtable->GetNode = PyDSOGetNode;
  strcpy(vtable->version, AI_VERSION);
  return true;
}

// ---

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
  switch (reason)
  {
  case DLL_PROCESS_ATTACH:
    PythonInterpreter::Get();
    break;
    
  case DLL_PROCESS_DETACH:
    PythonInterpreter::Finish();
    
  default:
    break;
  }
  
  return TRUE;
}

#else

__attribute__((constructor)) void _pythonDsoLoad(void)
{
  PythonInterpreter::Get();
}

__attribute__((destructor)) void _pythonDsoUnload(void)
{
  PythonInterpreter::Finish();
}

#endif