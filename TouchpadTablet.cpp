#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <windows.h>
#include <stdio.h>
#include <hidsdi.h>
#include <hidusage.h>
#include <vector>
#include <unordered_map>
#include <optional>
#include "resource.h"

#define WMAPP_NOTIFYCALLBACK (WM_APP + 1)
#define HID_USAGE_DIGITIZER_CONTACT_ID 0x51
#define HID_USAGE_DIGITIZER_CONTACT_COUNT 0x54

#define DEBUG_MODE 0

HWND hwnd;
HINSTANCE hInstance;
WNDCLASSEX wc;
NOTIFYICONDATA nid = {};

RECT bounds = { -1, -1, -1, -1 };
// units in mm. defaults are taken from my laptop
float width = 110;
float height = 51;
float awidth = 110;
float aheight = 51;

int swidth = GetSystemMetrics(SM_CXSCREEN);
int sheight = GetSystemMetrics(SM_CYSCREEN);

// Contact information parsed from the HID report descriptor.
struct contact_info
{
    USHORT link;
};

// The data for a touch event.
struct contact
{
    contact_info info;
    ULONG id;
    POINT point;
};

// Wrapper for malloc with unique_ptr semantics, to allow
// for variable-sized structures.
struct free_deleter { void operator()(void* ptr) { free(ptr); } };
template<typename T> using malloc_ptr = std::unique_ptr<T, free_deleter>;

// Device information, such as touch area bounds and HID offsets.
// This can be reused across HID events, so we only have to parse
// this info once.
struct device_info
{
    malloc_ptr<_HIDP_PREPARSED_DATA> preparsedData; // HID internal data
    USHORT linkContactCount = 0; // Link collection for number of contacts present
    std::vector<contact_info> contactInfo; // Link collection and touch area for each contact
};

// Caches per-device info for better performance
static std::unordered_map<HANDLE, device_info> g_devices;

// Holds the current primary touch point ID
static thread_local ULONG t_primaryContactID;

// Allocates a malloc_ptr with the given size. The size must be
// greater than or equal to sizeof(T).
template<typename T>
static malloc_ptr<T>
make_malloc(size_t size)
{
    T* ptr = (T*)malloc(size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return malloc_ptr<T>(ptr);
}

// C-style printf for debug output.
#if DEBUG_MODE
static void
vfdebugf(FILE* f, const char* fmt, va_list args)
{
    vfprintf(f, fmt, args);
    putc('\n', f);
}
static void
debugf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfdebugf(stderr, fmt, args);
    va_end(args);
}
#else
#define debugf(...) ((void)0)
#endif

std::vector<std::string> split(const std::string& s, char delim) {
    std::stringstream ss(s);
    std::string item;
    std::vector<std::string> elems;
    while (std::getline(ss, item, delim)) {
        elems.push_back(std::move(item));
    }
    return elems;
}

// Taken from Windows 7 SDK
BOOL AddNotificationIcon()
{
    nid.cbSize = sizeof(nid);
    nid.uID = 1;
    nid.hWnd = hwnd;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    nid.uCallbackMessage = WMAPP_NOTIFYCALLBACK;

    Shell_NotifyIcon(NIM_ADD, &nid);

    // NOTIFYICON_VERSION_4 is prefered
    nid.uVersion = NOTIFYICON_VERSION_4;
    return Shell_NotifyIcon(NIM_SETVERSION, &nid);
}

// Taken from Windows 7 SDK
void ShowContextMenu(HWND hwnd, POINT pt)
{
    HMENU hMenu = LoadMenu(hInstance, MAKEINTRESOURCE(IDR_MENU1));
    if (hMenu)
    {
        HMENU hSubMenu = GetSubMenu(hMenu, 0);
        if (hSubMenu)
        {
            // our window must be foreground before calling TrackPopupMenu or the menu will not disappear when the user clicks away
            SetForegroundWindow(hwnd);

            // respect menu drop alignment
            UINT uFlags = TPM_RIGHTBUTTON;
            if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0)
            {
                uFlags |= TPM_RIGHTALIGN;
            }
            else
            {
                uFlags |= TPM_LEFTALIGN;
            }

            TrackPopupMenuEx(hSubMenu, uFlags, pt.x, pt.y, hwnd, NULL);
        }
        DestroyMenu(hMenu);
    }
}

// On exit
void Clean() {
    Shell_NotifyIcon(NIM_DELETE, &nid);
    PostQuitMessage(0);
}

// Registers the specified window to receive touchpad HID events.
static void RegisterTouchpadInput()
{
    RAWINPUTDEVICE dev;
    dev.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
    dev.usUsage = HID_USAGE_DIGITIZER_TOUCH_PAD;
    dev.dwFlags = RIDEV_INPUTSINK;
    dev.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&dev, 1, sizeof(RAWINPUTDEVICE))) {
        throw;
    }
}

// Reads the raw input header for the given raw input handle.
static RAWINPUTHEADER GetRawInputHeader(HRAWINPUT hInput)
{
    RAWINPUTHEADER hdr;
    UINT size = sizeof(hdr);
    if (GetRawInputData(hInput, RID_HEADER, &hdr, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        throw;
    }
    return hdr;
}

// Reads the raw input data for the given raw input handle.
static malloc_ptr<RAWINPUT> GetRawInput(HRAWINPUT hInput, RAWINPUTHEADER hdr)
{
    malloc_ptr<RAWINPUT> input = make_malloc<RAWINPUT>(hdr.dwSize);
    UINT size = hdr.dwSize;
    if (GetRawInputData(hInput, RID_INPUT, input.get(), &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        throw;
    }
    return input;
}

// Gets info about a raw input device.
static RID_DEVICE_INFO GetRawInputDeviceInfo(HANDLE hDevice)
{
    RID_DEVICE_INFO info;
    info.cbSize = sizeof(RID_DEVICE_INFO);
    UINT size = sizeof(RID_DEVICE_INFO);
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, &info, &size) == (UINT)-1) {
        throw;
    }
    return info;
}

// Reads the preparsed HID report descriptor for the device
// that generated the given raw input.
static malloc_ptr<_HIDP_PREPARSED_DATA> GetHidPreparsedData(HANDLE hDevice)
{
    UINT size = 0;
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, nullptr, &size) == (UINT)-1) {
        throw;
    }
    malloc_ptr<_HIDP_PREPARSED_DATA> preparsedData = make_malloc<_HIDP_PREPARSED_DATA>(size);
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, preparsedData.get(), &size) == (UINT)-1) {
        throw;
    }
    return preparsedData;
}

// Returns all input button caps for the given preparsed
// HID report descriptor.
static std::vector<HIDP_BUTTON_CAPS> GetHidInputButtonCaps(PHIDP_PREPARSED_DATA preparsedData)
{
    NTSTATUS status;
    HIDP_CAPS caps;
    status = HidP_GetCaps(preparsedData, &caps);
    if (status != HIDP_STATUS_SUCCESS) {
        throw;
    }
    USHORT numCaps = caps.NumberInputButtonCaps;
    std::vector<HIDP_BUTTON_CAPS> buttonCaps(numCaps);
    status = HidP_GetButtonCaps(HidP_Input, &buttonCaps[0], &numCaps, preparsedData);
    if (status != HIDP_STATUS_SUCCESS) {
        throw;
    }
    buttonCaps.resize(numCaps);
    return buttonCaps;
}

// Returns all input value caps for the given preparsed
// HID report descriptor.
static std::vector<HIDP_VALUE_CAPS> GetHidInputValueCaps(PHIDP_PREPARSED_DATA preparsedData)
{
    NTSTATUS status;
    HIDP_CAPS caps;
    status = HidP_GetCaps(preparsedData, &caps);
    if (status != HIDP_STATUS_SUCCESS) {
        throw;
    }
    USHORT numCaps = caps.NumberInputValueCaps;
    std::vector<HIDP_VALUE_CAPS> valueCaps(numCaps);
    status = HidP_GetValueCaps(HidP_Input, &valueCaps[0], &numCaps, preparsedData);
    if (status != HIDP_STATUS_SUCCESS) {
        throw;
    }
    valueCaps.resize(numCaps);
    return valueCaps;
}

// Reads the pressed status of a single HID report button.
static bool GetHidUsageButton(
    HIDP_REPORT_TYPE reportType,
    USAGE usagePage,
    USHORT linkCollection,
    USAGE usage,
    PHIDP_PREPARSED_DATA preparsedData,
    PBYTE report,
    ULONG reportLen)
{
    ULONG numUsages = HidP_MaxUsageListLength(
        reportType,
        usagePage,
        preparsedData);
    std::vector<USAGE> usages(numUsages);
    NTSTATUS status = HidP_GetUsages(
        reportType,
        usagePage,
        linkCollection,
        &usages[0],
        &numUsages,
        preparsedData,
        (PCHAR)report,
        reportLen);
    if (status != HIDP_STATUS_SUCCESS) {
        throw;
    }
    usages.resize(numUsages);
    return std::find(usages.begin(), usages.end(), usage) != usages.end();
}

// Reads a single HID report value in logical units.
static ULONG GetHidUsageLogicalValue(
    HIDP_REPORT_TYPE reportType,
    USAGE usagePage,
    USHORT linkCollection,
    USAGE usage,
    PHIDP_PREPARSED_DATA preparsedData,
    PBYTE report,
    ULONG reportLen)
{
    ULONG value;
    NTSTATUS status = HidP_GetUsageValue(
        reportType,
        usagePage,
        linkCollection,
        usage,
        &value,
        preparsedData,
        (PCHAR)report,
        reportLen);
    if (status != HIDP_STATUS_SUCCESS) {
        throw;
    }
    return value;
}

// Reads a single HID report value in physical units.
static LONG GetHidUsagePhysicalValue(
    HIDP_REPORT_TYPE reportType,
    USAGE usagePage,
    USHORT linkCollection,
    USAGE usage,
    PHIDP_PREPARSED_DATA preparsedData,
    PBYTE report,
    ULONG reportLen)
{
    LONG value;
    NTSTATUS status = HidP_GetScaledUsageValue(
        reportType,
        usagePage,
        linkCollection,
        usage,
        &value,
        preparsedData,
        (PCHAR)report,
        reportLen);
    if (status != HIDP_STATUS_SUCCESS) {
        return -1;
    }
    return value;
}

// Gets the device info associated with the given raw input. Uses the
// cached info if available; otherwise parses the HID report descriptor
// and stores it into the cache.
static device_info& GetDeviceInfo(HANDLE hDevice)
{
    if (g_devices.count(hDevice)) {
        return g_devices.at(hDevice);
    }

    device_info dev;
    std::optional<USHORT> linkContactCount;
    dev.preparsedData = GetHidPreparsedData(hDevice);

    // Struct to hold our parser state
    struct contact_info_tmp
    {
        bool hasContactID = false;
        bool hasTip = false;
        bool hasX = false;
        bool hasY = false;
    };
    std::unordered_map<USHORT, contact_info_tmp> contacts;

    // Get the touch area for all the contacts. Also make sure that each one
    // is actually a contact, as specified by:
    // https://docs.microsoft.com/en-us/windows-hardware/design/component-guidelines/windows-precision-touchpad-required-hid-top-level-collections
    for (const HIDP_VALUE_CAPS& cap : GetHidInputValueCaps(dev.preparsedData.get())) {
        if (cap.IsRange || !cap.IsAbsolute) {
            continue;
        }

        if (cap.UsagePage == HID_USAGE_PAGE_GENERIC) {
            if (cap.NotRange.Usage == HID_USAGE_GENERIC_X) {
                contacts[cap.LinkCollection].hasX = true;
            }
            else if (cap.NotRange.Usage == HID_USAGE_GENERIC_Y) {
                contacts[cap.LinkCollection].hasY = true;
            }
        }
        else if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER) {
            if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_COUNT) {
                linkContactCount = cap.LinkCollection;
            }
            else if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_ID) {
                contacts[cap.LinkCollection].hasContactID = true;
            }
        }
    }

    for (const HIDP_BUTTON_CAPS& cap : GetHidInputButtonCaps(dev.preparsedData.get())) {
        if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER) {
            if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_TIP_SWITCH) {
                contacts[cap.LinkCollection].hasTip = true;
            }
        }
    }

    if (!linkContactCount.has_value()) {
        throw std::runtime_error("No contact count usage found");
    }
    dev.linkContactCount = linkContactCount.value();

    for (const auto& kvp : contacts) {
        USHORT link = kvp.first;
        const contact_info_tmp& info = kvp.second;
        if (info.hasContactID && info.hasTip && info.hasX && info.hasY) {
            debugf("Contact for device %p: link=%d",
                hDevice,
                link);
            dev.contactInfo.push_back({ link});
        }
    }

    return g_devices[hDevice] = std::move(dev);
}

// Reads all touch contact points from a raw input event.
static std::vector<contact> GetContacts(device_info& dev, RAWINPUT* input)
{
    std::vector<contact> contacts;

    DWORD sizeHid = input->data.hid.dwSizeHid;
    DWORD count = input->data.hid.dwCount;
    BYTE* rawData = input->data.hid.bRawData;
    if (count == 0) {
        debugf("Raw input contained no HID events");
        return contacts;
    }

    ULONG numContacts = GetHidUsageLogicalValue(
        HidP_Input,
        HID_USAGE_PAGE_DIGITIZER,
        dev.linkContactCount,
        HID_USAGE_DIGITIZER_CONTACT_COUNT,
        dev.preparsedData.get(),
        rawData,
        sizeHid);

    if (numContacts > dev.contactInfo.size()) {
        debugf("Device reported more contacts (%u) than we have links (%zu)", numContacts, dev.contactInfo.size());
        numContacts = (ULONG)dev.contactInfo.size();
    }

    // It's a little ambiguous as to whether contact count includes
    // released contacts. I interpreted the specs as a yes, but this
    // may require additional testing.
    for (ULONG i = 0; i < numContacts; ++i) {
        contact_info& info = dev.contactInfo[i];
        bool tip = GetHidUsageButton(
            HidP_Input,
            HID_USAGE_PAGE_DIGITIZER,
            info.link,
            HID_USAGE_DIGITIZER_TIP_SWITCH,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        if (!tip) {
            debugf("Contact has tip = 0, ignoring");
            continue;
        }

        ULONG id = GetHidUsageLogicalValue(
            HidP_Input,
            HID_USAGE_PAGE_DIGITIZER,
            info.link,
            HID_USAGE_DIGITIZER_CONTACT_ID,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        LONG x = GetHidUsagePhysicalValue(
            HidP_Input,
            HID_USAGE_PAGE_GENERIC,
            info.link,
            HID_USAGE_GENERIC_X,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        LONG y = GetHidUsagePhysicalValue(
            HidP_Input,
            HID_USAGE_PAGE_GENERIC,
            info.link,
            HID_USAGE_GENERIC_Y,
            dev.preparsedData.get(),
            rawData,
            sizeHid);

        if (x != -1 && y != -1)
            contacts.push_back({ info, id, { x, y } });
    }

    return contacts;
}

// Returns the primary contact for a given list of contacts. This is
// necessary since we are mapping potentially many touches to a single
// mouse position. Currently this just stores a global contact ID and
// uses that as the primary contact.
static contact GetPrimaryContact(const std::vector<contact>& contacts)
{
    for (const contact& contact : contacts) {
        if (contact.id == t_primaryContactID) {
            return contact;
        }
    }
    t_primaryContactID = contacts[0].id;
    return contacts[0];
}

void WriteCalibration() {
    std::ofstream calib;
    calib.open("tpcalib.dat");
    calib << bounds.left << std::endl;
    calib << bounds.right << std::endl;
    calib << bounds.top << std::endl;
    calib << bounds.bottom << std::endl;
    calib.close();
}

void ReadCalibration() {
    int i = 0;
    std::ifstream input("tpcalib.dat");
    if (input.good()) {
        for (std::string line; std::getline(input, line); )
        {
            switch (i) {
            case 0:
                bounds.left = std::stol(line.c_str());
                break;
            case 1:
                bounds.right = std::stol(line.c_str());
                break;
            case 2:
                bounds.top = std::stol(line.c_str());
                break;
            case 3:
                bounds.bottom = std::stol(line.c_str());
                break;
            }
            i++;
        }
        debugf("Loaded calibration %d %d %d %d", bounds.left, bounds.right, bounds.top, bounds.bottom);
    }
    else {
        MessageBox(hwnd, "Calibrate touchpad by touching each corner after clicking ok", "TouchpadTablet", MB_OK | MB_ICONQUESTION);
    }
}

void HandleCalibration(LONG x, LONG y) {
    if (x < bounds.left || bounds.left == -1) {
        bounds.left = x;
        WriteCalibration();
    }
    if (x > bounds.right || bounds.right == -1) {
        bounds.right = x;
        WriteCalibration();
    }
    if (y < bounds.top || bounds.top == -1) {
        bounds.top = y;
        WriteCalibration();
    }
    if (y > bounds.bottom || bounds.bottom == -1) {
        bounds.bottom = y;
        WriteCalibration();
    }
}

void ReadConfig() {
    std::string line;
    std::ifstream input("config.txt");
    if (input.good()) {
        for (std::string line; std::getline(input, line); )
        {
            if (line[0] == '#')
                continue;
            std::vector<std::string> s = split(line, '=');
            if (s.size() == 2) {
                if (s[0] == "Width")
                    width = std::stof(s[1].c_str());
                else if (s[0] == "Height")
                    height = std::stof(s[1].c_str());
                else if (s[0] == "AreaWidth")
                    awidth = std::stof(s[1].c_str());
                else if (s[0] == "AreaHeight")
                    aheight = std::stof(s[1].c_str());
            }
        }
        debugf("Loaded config.txt");
    }
}

// Handles a WM_INPUT event
static void HandleRawInput(WPARAM* wParam, LPARAM* lParam)
{
    INPUT event = { 0 };
    HRAWINPUT hInput = (HRAWINPUT)*lParam;
    RAWINPUTHEADER hdr = GetRawInputHeader(hInput);
    device_info& dev = GetDeviceInfo(hdr.hDevice);
    malloc_ptr<RAWINPUT> input = GetRawInput(hInput, hdr);
    std::vector<contact> contacts = GetContacts(dev, input.get());

    float newx = ((float)bounds.right - (float)bounds.left) * ((float)awidth / (float)width);
    float newy = ((float)bounds.bottom - (float)bounds.top) * ((float)aheight / (float)height);

    for (const contact& contact : contacts) {
        HandleCalibration(contact.point.x, contact.point.y);
    }
    if (contacts.empty()) {
        debugf("Found no contacts in input event");
        return;
    }

    contact contact = GetPrimaryContact(contacts);
    debugf("%d %d", contact.point.x, contact.point.y);
    double x = (contact.point.x - bounds.left - ((((float)bounds.right - (float)bounds.left) - newx) / 2)) * ((float)swidth / newx);
    double y = (contact.point.y - bounds.top - ((((float)bounds.bottom - (float)bounds.top) - newy) / 2)) * ((float)sheight / newy);

    event.type = INPUT_MOUSE;
    event.mi.dx = (long)((x * 65536) / swidth);
    event.mi.dy = (long)((y * 65536) / sheight);
    event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &event, sizeof(INPUT));
}

BOOL HasPrecisionTouchpad() {
    std::vector<RAWINPUTDEVICELIST> devices(64);

    while (true) {
        UINT numDevices = (UINT)devices.size();
        UINT ret = GetRawInputDeviceList(&devices[0], &numDevices, sizeof(RAWINPUTDEVICELIST));
        if (ret != (UINT)-1) {
            devices.resize(ret);
            break;
        }
        else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            devices.resize(numDevices);
        }
        else {
            return false;
        }
    }

    for (RAWINPUTDEVICELIST dev : devices) {
        RID_DEVICE_INFO info;
        info.cbSize = sizeof(RID_DEVICE_INFO);
        UINT size = sizeof(RID_DEVICE_INFO);
        if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICEINFO, &info, &size) == (UINT)-1) {
            continue;
        }
        if (info.dwType == RIM_TYPEHID &&
            info.hid.usUsagePage == HID_USAGE_PAGE_DIGITIZER &&
            info.hid.usUsage == HID_USAGE_DIGITIZER_TOUCH_PAD) {
            device_info& info = GetDeviceInfo(dev.hDevice);
            if (!info.contactInfo.empty()) {
                debugf("Detected touchpad with handle %p, %zu", dev.hDevice, info.contactInfo.size());
                return true;
            }
            else
                break;
        }
    }
    return false;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg)
    {
        case WMAPP_NOTIFYCALLBACK:
            if (LOWORD(lParam) == WM_CONTEXTMENU)
            {
                POINT const pt = { LOWORD(wParam), HIWORD(wParam) };
                ShowContextMenu(hwnd, pt);
            }
            break;
        case WM_INPUT:
            HandleRawInput(&wParam, &lParam);
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_EXIT_EXIT)
                Clean();
            break;
        case WM_DESTROY:
            Clean();
            break;
        default:
            return DefWindowProc(hwnd, Msg, wParam, lParam);
    }
    return 0;
}

static void
StartDebugMode()
{
#if DEBUG_MODE
    FreeConsole();
    AllocConsole();
#pragma warning(push)
#pragma warning(disable:4996)
    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
#pragma warning(pop)
#endif
}

int APIENTRY wWinMain(_In_ HINSTANCE _hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    MSG msg;

    hInstance = _hInstance;
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "UWU_CLASS";

    if (RegisterClassEx(&wc)) {
        hwnd = CreateWindowEx(0, "UWU_CLASS", "UWU", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    StartDebugMode();

    if (!HasPrecisionTouchpad()) {
        debugf("No precision touchpad detected");
        MessageBox(NULL, "No precision touchpad detected", "TouchpadTablet", MB_OK | MB_ICONERROR);
        Clean();
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS); // Reduce input lag
    AddNotificationIcon();
    ReadConfig();
    ReadCalibration();
    RegisterTouchpadInput();

    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}