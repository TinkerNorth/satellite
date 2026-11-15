// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Text;
using Satellite.HmHelper.Core;

namespace Satellite.HmHelper;

// The device tree as the Configuration Manager reads it. It never opens a
// device: a handle would veto a removal Windows asks for, and keep a failed
// UMDF stack from restarting.
internal sealed class WindowsDevnodeTree : IDevnodeTree
{
    private const int GuidChars = 38;

    public DevnodeState Locate(string instanceId) =>
        CfgMgr.CM_Locate_DevNodeW(out uint node, instanceId, CfgMgr.CM_LOCATE_DEVNODE_NORMAL) == CfgMgr.CR_SUCCESS
            ? Read(node)
            : default;

    public DevnodeState FirstChild(DevnodeState parent) =>
        parent.Located && CfgMgr.CM_Get_Child(out uint child, parent.Handle, 0) == CfgMgr.CR_SUCCESS
            ? Read(child)
            : default;

    public DevnodeState ChildOfClass(DevnodeState parent, Guid setupClass)
    {
        if (!parent.Located) return default;
        uint found = CfgMgr.CM_Get_Child(out uint node, parent.Handle, 0);
        while (found == CfgMgr.CR_SUCCESS)
        {
            if (ClassOf(node) == setupClass) return Read(node);
            found = CfgMgr.CM_Get_Sibling(out node, node, 0);
        }
        return default;
    }

    public static string? InstanceIdOf(DevnodeState node)
    {
        var id = new char[CfgMgr.MAX_DEVICE_ID_LEN];
        bool named = node.Located && CfgMgr.CM_Get_Device_IDW(node.Handle, id, (uint)id.Length, 0) == CfgMgr.CR_SUCCESS;
        return named ? new string(id).TrimEnd('\0') : null;
    }

    // A devnode whose status cannot be read has left the tree since it was found.
    private static DevnodeState Read(uint node) =>
        CfgMgr.CM_Get_DevNode_Status(out uint status, out uint problem, node, 0) == CfgMgr.CR_SUCCESS
            ? new DevnodeState(true, status, problem, node)
            : default;

    private static Guid? ClassOf(uint node)
    {
        var buffer = new byte[(GuidChars + 1) * 2];
        uint length = (uint)buffer.Length;
        uint read = CfgMgr.CM_Get_DevNode_Registry_PropertyW(node, CfgMgr.CM_DRP_CLASSGUID, out _, buffer, ref length, 0);
        if (read != CfgMgr.CR_SUCCESS) return null;
        string text = Encoding.Unicode.GetString(buffer, 0, (int)length).TrimEnd('\0');
        return Guid.TryParse(text, out Guid setupClass) ? setupClass : null;
    }
}
