#pragma once
#include <windows.h>
inline UINT MsiGetPropertyW(MSIHANDLE,LPCWSTR n,LPWSTR b,DWORD*size){auto it=world->props.find(n);std::wstring v=it==world->props.end()?L"":it->second;if(*size<=v.size()){*size=(DWORD)v.size();return ERROR_MORE_DATA;}std::wcscpy(b,v.c_str());*size=(DWORD)v.size();return 0;}
inline UINT MsiSetPropertyW(MSIHANDLE,LPCWSTR n,LPCWSTR v){world->props[n]=v;return 0;}
inline MSIHANDLE MsiCreateRecord(UINT){return 1;}
inline std::wstring record_field;
inline UINT MsiRecordSetStringW(MSIHANDLE,UINT i,LPCWSTR v){if(i==1)record_field=v;return 0;}
inline int MsiProcessMessage(MSIHANDLE,int kind,MSIHANDLE){(kind==INSTALLMESSAGE_ERROR?world->shown:world->log).push_back(record_field);return 0;}
inline UINT MsiCloseHandle(MSIHANDLE){return 0;}
