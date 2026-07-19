// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later

namespace WinSquish.Interop;

/// <summary>Thrown when a libsquish call returns a non-OK status.</summary>
public sealed class SquishException : Exception
{
    internal SquishStatus Status { get; }

    internal SquishException(SquishStatus status, string operation)
        : base($"{operation} failed: {SquishNative.StrError(status)} (code {(int)status})")
    {
        Status = status;
    }

    /// <summary>Throw if <paramref name="code"/> is not <see cref="SquishStatus.Ok"/>.</summary>
    internal static void Check(int code, string operation)
    {
        var status = (SquishStatus)code;
        if (status != SquishStatus.Ok)
            throw new SquishException(status, operation);
    }
}
