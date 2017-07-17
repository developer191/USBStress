// USBStress.cpp : Defines the entry point for the application.
//

#include "stdafx.h"
#include <Windows.h>
#include <SetupAPI.h>
#include <initguid.h>
#include <devguid.h>
#include <usbiodef.h>
#include <devpkey.h>
#include <vector>
#include <string>
#include <assert.h>
#include <atlcomcli.h>
#include <locale>
#include <codecvt>
#include <Cfgmgr32.h>

#include <nana/gui/wvl.hpp>
#include <nana/gui/widgets/group.hpp>
#include <nana/gui/widgets/button.hpp>
#include <nana/gui/widgets/label.hpp>
#include <nana/gui/widgets/textbox.hpp>

using namespace std;

template <class T> inline void SafeFree(T*& pT)
{
    if (pT != nullptr)
    {
        free(pT);
        pT = nullptr;
    }
}

typedef struct _USB_DEV_ENTRY
{
    UINT vendorId;
    UINT productId;
    std::wstring instancePath;
}USB_DEV_ENTRY, *PUSB_DEV_ENTRY;

typedef struct _THREAD_DATA
{
    HANDLE evTerm;
    HANDLE tHandle;
    DWORD tID;
    UINT vid;
    UINT pid;
    vector<USB_DEV_ENTRY> devList;
    UINT testModePnp;
    UINT testLoops;
    UINT intervalS;
}THREAD_DATA, *PTHREAD_DATA;

typedef struct _SETUPDI_DEV_CONTEXT
{
    HDEVINFO hDevInfo;
    SP_DEVINFO_DATA devInfoData;
}SETUPDI_DEV_CONTEXT, *PSETUPDI_DEV_CONTEXT;

void EnumerateDevList(UINT vid, UINT pid, vector<USB_DEV_ENTRY> &dList)
{
    //HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_USB, 0, 0, DIGCF_PRESENT);
    HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_DEVICE, 0, 0, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (INVALID_HANDLE_VALUE != hDevInfo)
    {
        SP_DEVINFO_DATA devInfoData = { 0 };
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        PBYTE pDataBuf = nullptr;
        DWORD bufSize = 0;
        for (DWORD devIdx = 0; SetupDiEnumDeviceInfo(hDevInfo, devIdx, &devInfoData); devIdx++)
        {
            DWORD err;
            DEVPROPTYPE PropType;

            // Get device instance path, vendor ID, device ID
            SafeFree(pDataBuf);
            bufSize = 0;
            while (!SetupDiGetDeviceProperty(
                hDevInfo,
                &devInfoData,
                &DEVPKEY_Device_InstanceId,
                &PropType,
                pDataBuf,
                bufSize,
                &bufSize,
                0))
            {
                err = GetLastError();
                if (err == ERROR_INSUFFICIENT_BUFFER)
                {
                    SafeFree(pDataBuf);
                    pDataBuf = (PBYTE)malloc(bufSize);
                    RtlZeroMemory(pDataBuf, bufSize);
                }
                else
                {
                    break;
                }
            }

            if (wcslen((PWCHAR)pDataBuf))
            {
                USB_DEV_ENTRY usbDev;
                WCHAR idBuf[5] = { 0 };
                wcsncpy_s(idBuf, 5, (PWCHAR)pDataBuf + 8, 4);
                usbDev.vendorId = (UINT)wcstol(idBuf, nullptr, 16);
                wcsncpy_s(idBuf, 5, (PWCHAR)pDataBuf + 17, 4);
                usbDev.productId = (UINT)wcstol(idBuf, nullptr, 16);
                if ((usbDev.vendorId == vid) && (usbDev.productId == pid))
                {
                    usbDev.instancePath = std::wstring((PWCHAR)pDataBuf);
                    dList.push_back(usbDev);
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }
}

DWORD WINAPI PnPTestThread(_In_ void *pParam)
{
    PTHREAD_DATA pTData = reinterpret_cast<PTHREAD_DATA>(pParam);
    BOOL isSuccess = FALSE;
    UINT testRound = 0;
    UINT internalMS = pTData->intervalS * 1000;
    DWORD err = 0;

    // PnP OP to test
    vector<SP_PROPCHANGE_PARAMS> pnpOpList;
    pnpOpList.clear();
    SP_PROPCHANGE_PARAMS pnpOp = {0};
    pnpOp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pnpOp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pnpOp.Scope = DICS_FLAG_GLOBAL;
    pnpOp.StateChange = DICS_DISABLE;
    pnpOpList.push_back(pnpOp);
    pnpOp.StateChange = DICS_ENABLE;
    pnpOpList.push_back(pnpOp);

    // Prepare for all devices
    vector<SETUPDI_DEV_CONTEXT> diDevCtxList;
    diDevCtxList.clear();
    for (vector<USB_DEV_ENTRY>::iterator dev = pTData->devList.begin(); dev != pTData->devList.end(); dev++)
    {
        SETUPDI_DEV_CONTEXT devCtx;
        RtlZeroMemory(&devCtx.devInfoData, sizeof(devCtx.devInfoData));
        devCtx.devInfoData.cbSize = sizeof(devCtx.devInfoData);
        devCtx.hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
        if(!SetupDiOpenDeviceInfo(devCtx.hDevInfo, (PCWSTR)(dev->instancePath.c_str()), nullptr, DIOD_INHERIT_CLASSDRVS, &devCtx.devInfoData))
        {
            err = GetLastError();
            assert(0);
        }
        diDevCtxList.push_back(devCtx);
    }

    while ((WaitForSingleObjectEx(pTData->evTerm, 0, FALSE) == WAIT_TIMEOUT) && (testRound++ < pTData->testLoops))
    {
        for (vector<SP_PROPCHANGE_PARAMS>::iterator op = pnpOpList.begin(); op != pnpOpList.end(); op++)
        {
            for (vector<SETUPDI_DEV_CONTEXT>::iterator devCtx = diDevCtxList.begin(); devCtx != diDevCtxList.end(); devCtx++)
            {
                if (!SetupDiSetClassInstallParams(devCtx->hDevInfo, &devCtx->devInfoData, &op->ClassInstallHeader, sizeof(*op)))
                {
                    err = GetLastError();
                }
                if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devCtx->hDevInfo, &devCtx->devInfoData))
                {
                    err = GetLastError();
                }
                Sleep(internalMS);
            }
        }
    }

    // Enable on Test done
    for (vector<SETUPDI_DEV_CONTEXT>::iterator devCtx = diDevCtxList.begin(); devCtx != diDevCtxList.end(); devCtx++)
    {
        pnpOp.StateChange = DICS_ENABLE;
        if (!SetupDiSetClassInstallParams(devCtx->hDevInfo, &devCtx->devInfoData, &pnpOp.ClassInstallHeader, sizeof(pnpOp)))
        {
            err = GetLastError();
        }
        if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devCtx->hDevInfo, &devCtx->devInfoData))
        {
            err = GetLastError();
        }
        Sleep(internalMS);
    }

    // Clean up for all devices
    for (vector<SETUPDI_DEV_CONTEXT>::iterator devCtx = diDevCtxList.begin(); devCtx != diDevCtxList.end(); devCtx++)
    {
        isSuccess = SetupDiDestroyDeviceInfoList(devCtx->hDevInfo);
    }
    diDevCtxList.clear();
    return 0;
}

DWORD WINAPI ReInstallTestThread(_In_ void *pParam)
{
    PTHREAD_DATA pTData = reinterpret_cast<PTHREAD_DATA>(pParam);
    BOOL isSuccess = FALSE;
    UINT testRound = 0;
    UINT internalMS = pTData->intervalS * 1000;
    DWORD err = 0;

    CONFIGRET cmRet = CR_SUCCESS;
    while ((WaitForSingleObjectEx(pTData->evTerm, 0, FALSE) == WAIT_TIMEOUT) && (testRound++ < pTData->testLoops))
    {
        // Rescan device
        DEVINST cmDevInst;
        cmRet = CM_Locate_DevNode_Ex(&cmDevInst, NULL, CM_LOCATE_DEVNODE_NORMAL, NULL);
        cmRet = CM_Reenumerate_DevNode_Ex(cmDevInst, CM_REENUMERATE_NORMAL, NULL);
        Sleep(internalMS);

        // Enmu device
        vector<USB_DEV_ENTRY> devList;
        EnumerateDevList(pTData->vid, pTData->pid, devList);

        // Remove Device
        for (vector<USB_DEV_ENTRY>::iterator dev = devList.begin(); dev != devList.end(); dev++)
        {
            HDEVINFO hDevInfo;
            SP_DEVINFO_DATA devInfoData;
            RtlZeroMemory(&devInfoData, sizeof(devInfoData));
            devInfoData.cbSize = sizeof(devInfoData);
            hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
            if (!SetupDiOpenDeviceInfo(hDevInfo, (PCWSTR)(dev->instancePath.c_str()), nullptr, DIOD_INHERIT_CLASSDRVS, &devInfoData))
            {
                err = GetLastError();
                assert(0);
            }
            if (!SetupDiRemoveDevice(hDevInfo, &devInfoData))
            {
                err = GetLastError();
                assert(0);
            }
            if (!SetupDiDestroyDeviceInfoList(hDevInfo))
            {
                err = GetLastError();
                assert(0);
            }
            Sleep(internalMS);
        }
    }

    // Rescan device on Test done
    DEVINST cmDevInst;
    cmRet = CM_Locate_DevNode_Ex(&cmDevInst, NULL, CM_LOCATE_DEVNODE_NORMAL, NULL);
    cmRet = CM_Reenumerate_DevNode_Ex(cmDevInst, CM_REENUMERATE_NORMAL, NULL);

    return 0;
}

void StartTest(PTHREAD_DATA pTData, BOOL isStart)
{
    vector<USB_DEV_ENTRY> devList;
    devList.clear();
    pTData->devList.clear();
    if (pTData)
    {
        if (isStart)
        {
            if (pTData->testModePnp)
            {
                EnumerateDevList(pTData->vid, pTData->pid, devList);
                pTData->devList = devList;
                ResetEvent(pTData->evTerm);
                pTData->tHandle = CreateThread(nullptr, 0, PnPTestThread, pTData, 0, &pTData->tID);
            }
            else
            {
                ResetEvent(pTData->evTerm);
                pTData->tHandle = CreateThread(nullptr, 0, ReInstallTestThread, pTData, 0, &pTData->tID);
            }
        }
        else
        {
            SetEvent(pTData->evTerm);
            WaitForSingleObjectEx(pTData->tHandle, INFINITE, FALSE);
        }
    }
}


int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    BOOL isTestRun = FALSE;
    THREAD_DATA tData;
    tData.devList.clear();
    tData.evTerm = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    tData.tHandle = nullptr;
    tData.testModePnp = 0;
    tData.testLoops = 0;

    using namespace nana;

    form fm{ API::make_center(400,200) };
    fm.caption("USB Device Stress Test");
    //fm.bgcolor(colors::mint_cream );
    place plc(fm);
    std::vector<std::unique_ptr<button>> btns;
    // the most external widgets
    //label  out{ fm,  "This label is out of any group" };
    group layoutMain{ fm,  "", true };
    plc.div("vert gap=10 margin=5 <all> ");
    //plc["lab"] << out;
    plc["all"] << layoutMain;

    group grpDev{ layoutMain, ("USB Device"), true };
    label labelVID{ grpDev, "VID: "};
    label labelPID{ grpDev, "PID: "};
    textbox textVID{ grpDev, "0123"};
    textbox textPID{ grpDev, "ABCD"};
    label labelEmpty{ grpDev, "          " };
    grpDev.div("gap=5 margin=3 <lableVID weight=30><textVID><lableEmpty1><lablePID weight=30><textPID><lableEmpty2>");
    grpDev["lableVID"] << labelVID;
    grpDev["textVID"] << textVID;
    grpDev["lableEmpty1"] << labelEmpty;
    grpDev["lablePID"] << labelPID;
    grpDev["textPID"] << textPID;
    grpDev["lableEmpty2"] << labelEmpty;

    group grpSettings{ layoutMain, ("Test Mode"), true };
    checkbox modePNP{ grpSettings, ("PnP"), true };
    checkbox modeReinstall{ grpSettings, ("ReInstall"), true };
    radio_group modeGroup;
    modeGroup.add(modePNP);
    modeGroup.add(modeReinstall);
    modePNP.check(true);
    label labelLoop{ grpSettings,  "Loop:" };
    textbox textLoop{ grpSettings, "1000" };
    label labelInterval{ grpSettings,  "Interval(s):" };
    textbox textInterval{ grpSettings, "2" };

    grpSettings.div("<margin=2 gap= 4 <modePNP weight=60> <modeReinstall weight=80> <lableLoop weight=35> <inputLoop weight=50> <lableInterval weight=70> <inputInterval weight=30>>");
    grpSettings["modePNP"] << modePNP;
    grpSettings["modeReinstall"] << modeReinstall;
    grpSettings["lableLoop"] << labelLoop;
    grpSettings["inputLoop"] << textLoop;
    grpSettings["lableInterval"] << labelInterval;
    grpSettings["inputInterval"] << textInterval;

    group grpControl{ layoutMain, ("Control"), true };
    button btnStart{ grpControl, "Start" };
    grpControl.div("<margin=2 gap= 2 <all>>");
    grpControl["all"] << btnStart;
    btnStart.events().click([&isTestRun, &tData, &textVID, &textPID, &modePNP, &modeReinstall, &textLoop, &textInterval, &btnStart]()
    {
        isTestRun = !isTestRun;
        if (isTestRun)
        {
            btnStart.caption("Stop");
        }
        else
        {
            btnStart.caption("Start");
        }

        tData.vid = wcstol(textVID.caption_native().c_str(), nullptr, 16);
        tData.pid = wcstol(textPID.caption_native().c_str(), nullptr, 16);
        if (modePNP.checked())
        {
            tData.testModePnp = 1;
        }
        else
        {
            tData.testModePnp = 0;
        }
        tData.testLoops = (UINT)wcstol(textLoop.caption_native().c_str(), nullptr, 10);
        tData.intervalS = (UINT)wcstol(textInterval.caption_native().c_str(), nullptr, 10);

        StartTest(&tData, isTestRun);
    });

    layoutMain.div("vert gap=5 margin=5 <_grpDev><_grpSettings><_grpControl>");
    layoutMain["_grpDev"] << grpDev;
    layoutMain["_grpSettings"] << grpSettings;
    layoutMain["_grpControl"] << grpControl;
    plc.collocate();
    //grp1.plc.collocate();    // OK
    fm.show();
    exec();

    SetEvent(tData.evTerm);
    WaitForSingleObjectEx(tData.tHandle, INFINITE, FALSE);
    CloseHandle(tData.evTerm);
}