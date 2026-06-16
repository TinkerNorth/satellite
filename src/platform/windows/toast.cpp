// SPDX-License-Identifier: LGPL-3.0-or-later
#include "toast.h"
#include "net/pairing_service.h"
#include "shell_integration.h" // kAppUserModelID

#include <roapi.h>
#include <winstring.h>

#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>

#include <string>

using namespace ABI::Windows::Data::Xml::Dom;
using namespace ABI::Windows::UI::Notifications;

namespace {

// Minimal RAII for a COM interface pointer (MinGW has no WRL ComPtr).
template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    void** vput() { return reinterpret_cast<void**>(&p); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Minimal RAII for an HSTRING.
struct HStr {
    HSTRING h = nullptr;
    explicit HStr(const std::wstring& s) {
        WindowsCreateString(s.c_str(), static_cast<UINT32>(s.size()), &h);
    }
    HStr(const HStr&) = delete;
    HStr& operator=(const HStr&) = delete;
    ~HStr() {
        if (h) WindowsDeleteString(h);
    }
    HSTRING get() const { return h; }
};

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Escape the five XML entities so a device name can't break the document or
// inject markup.
std::wstring xmlEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
        case L'&':
            out += L"&amp;";
            break;
        case L'<':
            out += L"&lt;";
            break;
        case L'>':
            out += L"&gt;";
            break;
        case L'"':
            out += L"&quot;";
            break;
        case L'\'':
            out += L"&apos;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

} // namespace

bool showActionablePairToast(const std::string& deviceId, const std::string& deviceName,
                             const std::string& clientIP, const std::string& pin) {
    const std::wstring name =
        xmlEscape(toWide(deviceName.empty() ? std::string("A device") : deviceName));
    const std::wstring ip = xmlEscape(toWide(clientIP));
    const std::wstring wpin = xmlEscape(toWide(pin));
    const std::wstring wid = toWide(deviceId); // 32 hex chars — no escaping needed

    // Body shows the PIN so the operator confirms it by sight before accepting.
    const std::wstring xml = L"<toast activationType=\"protocol\" launch=\"satellite-pair:open\">"
                             L"<visual><binding template=\"ToastGeneric\">"
                             L"<text>Pairing request</text>"
                             L"<text>" +
                             name + L" (" + ip + L") wants to pair. PIN on the device: " + wpin +
                             L"</text>"
                             L"</binding></visual>"
                             L"<actions>"
                             L"<action content=\"Accept\" activationType=\"protocol\" "
                             L"arguments=\"satellite-pair:accept/" +
                             wid +
                             L"\"/>"
                             L"<action content=\"Reject\" activationType=\"protocol\" "
                             L"arguments=\"satellite-pair:reject/" +
                             wid +
                             L"\"/>"
                             L"</actions></toast>";

    Com<IInspectable> inspectable;
    HStr xmlDocClass(L"Windows.Data.Xml.Dom.XmlDocument");
    if (FAILED(RoActivateInstance(xmlDocClass.get(), inspectable.put())) || !inspectable) {
        return false;
    }
    Com<IXmlDocumentIO> xmlIo;
    if (FAILED(inspectable->QueryInterface(__uuidof(IXmlDocumentIO), xmlIo.vput())) || !xmlIo) {
        return false;
    }
    HStr xmlStr(xml);
    if (FAILED(xmlIo->LoadXml(xmlStr.get()))) return false;
    Com<IXmlDocument> xmlDoc;
    if (FAILED(inspectable->QueryInterface(__uuidof(IXmlDocument), xmlDoc.vput())) || !xmlDoc) {
        return false;
    }

    // Notifier for our AUMID (must match the installer shortcut, else toasts drop).
    Com<IToastNotificationManagerStatics> mgr;
    HStr mgrClass(L"Windows.UI.Notifications.ToastNotificationManager");
    if (FAILED(RoGetActivationFactory(mgrClass.get(), __uuidof(IToastNotificationManagerStatics),
                                      mgr.vput())) ||
        !mgr) {
        return false;
    }
    Com<IToastNotifier> notifier;
    HStr aumid(shell_integration::kAppUserModelID);
    if (FAILED(mgr->CreateToastNotifierWithId(aumid.get(), notifier.put())) || !notifier) {
        return false;
    }

    Com<IToastNotificationFactory> factory;
    HStr toastClass(L"Windows.UI.Notifications.ToastNotification");
    if (FAILED(RoGetActivationFactory(toastClass.get(), __uuidof(IToastNotificationFactory),
                                      factory.vput())) ||
        !factory) {
        return false;
    }
    Com<IToastNotification> toast;
    if (FAILED(factory->CreateToastNotification(xmlDoc.p, toast.put())) || !toast) return false;

    return SUCCEEDED(notifier->Show(toast.p));
}

void registerPairProtocol() {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return;

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\satellite-pair", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const wchar_t* desc = L"URL:Satellite Pairing";
    RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(desc),
                   static_cast<DWORD>((wcslen(desc) + 1) * sizeof(wchar_t)));
    // The empty "URL Protocol" value is what marks the key as a URL scheme.
    RegSetValueExW(key, L"URL Protocol", 0, REG_SZ, reinterpret_cast<const BYTE*>(L""),
                   sizeof(wchar_t));

    HKEY cmdKey = nullptr;
    if (RegCreateKeyExW(key, L"shell\\open\\command", 0, nullptr, 0, KEY_WRITE, nullptr, &cmdKey,
                        nullptr) == ERROR_SUCCESS) {
        std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" \"%1\"";
        RegSetValueExW(cmdKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(cmdKey);
    }
    RegCloseKey(key);
}

void handlePairProtocolUri(const std::string& uri) {
    const std::string scheme = "satellite-pair:";
    if (uri.rfind(scheme, 0) != 0) return;
    std::string rest = uri.substr(scheme.size()); // "accept/<id>", "reject/<id>", or "open"
    const auto slash = rest.find('/');
    if (slash == std::string::npos) return; // e.g. the body "open" — nothing to do
    const std::string action = rest.substr(0, slash);
    std::string deviceId = rest.substr(slash + 1);
    while (!deviceId.empty() && (deviceId.back() == '/' || deviceId.back() == ' ' ||
                                 deviceId.back() == '\r' || deviceId.back() == '\n')) {
        deviceId.pop_back();
    }
    if (action == "accept") {
        if (!confirmPairing(deviceId)) {
            shell_integration::showToast("Pairing not completed",
                                         "That request expired before you accepted it. Ask the "
                                         "device to tap Pair again.");
        }
    } else if (action == "reject") {
        declinePairing(deviceId);
    }
}
