// SPDX-License-Identifier: LGPL-3.0-or-later
using System.Reflection;
using System.Reflection.Emit;

namespace Satellite.HmHelper.Core;

// Reads a method's IL one instruction at a time, for the checks that pin what
// a vendored assembly does inside one method (check-sdk): a literal the SDK
// builds an id from is spelt out there and nowhere else.
public static class IlScan
{
    private const BindingFlags Every = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static |
                                       BindingFlags.Instance | BindingFlags.DeclaredOnly;

    private static readonly Dictionary<short, OpCode> ByValue = OpCodesByValue();

    // Whether one method of `type` both loads `literal` with an ldstr and calls
    // `factory`'s method named `create`: the method that makes what `factory`
    // makes from that literal. A literal loaded in one method and the call made
    // in another is not that.
    public static bool BuildsFrom(Type type, Type factory, string create, string literal)
    {
        foreach (MethodBase method in type.GetMethods(Every).Cast<MethodBase>().Concat(type.GetConstructors(Every)))
        {
            bool loads = false;
            bool calls = false;
            foreach ((OpCode code, int token) in Instructions(method))
            {
                if (code == OpCodes.Ldstr)
                    loads |= StringAt(method, token) == literal;
                else if (code == OpCodes.Call || code == OpCodes.Callvirt)
                    calls |= MethodAt(method, token) is { } callee && callee.Name == create && callee.DeclaringType == factory;
            }
            if (loads && calls) return true;
        }
        return false;
    }

    // Each instruction of `method`'s body, with its operand when that is a
    // metadata token and 0 when it is not. The walk follows each opcode's
    // operand size, so an operand's bytes are never read as an opcode.
    public static IEnumerable<(OpCode Code, int Token)> Instructions(MethodBase method)
    {
        byte[] il = method.GetMethodBody()?.GetILAsByteArray() ?? [];
        int at = 0;
        while (at < il.Length)
        {
            bool twoBytes = il[at] == 0xFE && at + 1 < il.Length;
            short value = twoBytes ? unchecked((short)(0xFE00 | il[at + 1])) : il[at];
            at += twoBytes ? 2 : 1;
            if (!ByValue.TryGetValue(value, out OpCode code)) yield break;
            bool token = IsToken(code.OperandType) && at + 4 <= il.Length;
            yield return (code, token ? BitConverter.ToInt32(il, at) : 0);
            at += OperandSize(code.OperandType, il, at);
        }
    }

    private static int OperandSize(OperandType operand, byte[] il, int at) => operand switch
    {
        OperandType.InlineNone => 0,
        OperandType.ShortInlineBrTarget or OperandType.ShortInlineI or OperandType.ShortInlineVar => 1,
        OperandType.InlineVar => 2,
        OperandType.InlineI8 or OperandType.InlineR => 8,
        OperandType.InlineSwitch => at + 4 <= il.Length ? 4 + 4 * BitConverter.ToInt32(il, at) : il.Length,
        _ => 4,
    };

    private static bool IsToken(OperandType operand) =>
        operand is OperandType.InlineField or OperandType.InlineMethod or OperandType.InlineSig or
            OperandType.InlineString or OperandType.InlineTok or OperandType.InlineType;

    private static Dictionary<short, OpCode> OpCodesByValue()
    {
        var byValue = new Dictionary<short, OpCode>();
        foreach (FieldInfo field in typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static))
        {
            if (field.GetValue(null) is OpCode code) byValue.TryAdd(code.Value, code);
        }
        return byValue;
    }

    private static string? StringAt(MethodBase method, int token)
    {
        try { return method.Module.ResolveString(token); }
        catch (ArgumentException) { return null; }
    }

    // A call inside a generic type or method names its callee in that context.
    private static MethodBase? MethodAt(MethodBase method, int token)
    {
        Type[]? typeArguments = method.DeclaringType is { IsGenericType: true } type ? type.GetGenericArguments() : null;
        Type[]? methodArguments = method.IsGenericMethod ? method.GetGenericArguments() : null;
        try { return method.Module.ResolveMethod(token, typeArguments, methodArguments); }
        catch (ArgumentException) { return null; }
    }
}
