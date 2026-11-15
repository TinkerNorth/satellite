// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Runtime.InteropServices;

namespace Satellite.HmHelper;

// The Configuration Manager calls the device watch and its selftest make
// (cfgmgr32.h, cfg.h).
internal static class CfgMgr
{
    public const uint CR_SUCCESS = 0x00000000;
    public const uint CR_REMOVE_VETOED = 0x00000017;
    public const uint CM_LOCATE_DEVNODE_NORMAL = 0x00000000;
    public const uint CM_DRP_CLASSGUID = 0x00000009;
    public const uint CM_DISABLE_UI_NOT_OK = 0x00000004;
    public const uint CM_REMOVE_NO_RESTART = 0x00000002;
    public const int MAX_DEVICE_ID_LEN = 200;

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    public static extern uint CM_Locate_DevNodeW(out uint devInst, string deviceId, uint flags);

    [DllImport("cfgmgr32.dll")]
    public static extern uint CM_Get_DevNode_Status(out uint status, out uint problem, uint devInst, uint flags);

    [DllImport("cfgmgr32.dll")]
    public static extern uint CM_Get_Child(out uint child, uint devInst, uint flags);

    [DllImport("cfgmgr32.dll")]
    public static extern uint CM_Get_Sibling(out uint sibling, uint devInst, uint flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    public static extern uint CM_Get_DevNode_Registry_PropertyW(uint devInst, uint property, out uint regType,
                                                                byte[]? buffer, ref uint length, uint flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    public static extern uint CM_Get_Device_IDW(uint devInst, char[] buffer, uint length, uint flags);

    [DllImport("cfgmgr32.dll")]
    public static extern uint CM_Disable_DevNode(uint devInst, uint flags);

    [DllImport("cfgmgr32.dll")]
    public static extern uint CM_Enable_DevNode(uint devInst, uint flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    public static extern uint CM_Query_And_Remove_SubTreeW(uint devInst, out int vetoType, char[]? vetoName,
                                                           uint nameLength, uint flags);
}
