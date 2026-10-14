# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
  [Parameter(Mandatory=$true)][int]$GameProcessId,
  [Parameter(Mandatory=$true)][string]$GameExecutable,
  [Parameter(Mandatory=$true)][string]$RequestPath,
  [Parameter(Mandatory=$true)][string]$ResultPath
)
$ErrorActionPreference = 'Stop'
$result = @{ passed=$false; processId=$GameProcessId }
try {
  $requestDirectory = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($RequestPath))
  if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($ResultPath)) -ne $requestDirectory) { throw 'Window request and result must share the evidence directory.' }
  $request = Get-Content -LiteralPath $RequestPath -Raw | ConvertFrom-Json
  if ($request.operation -notin @('inspect','resize','minimize','restore','capture')) { throw 'Unknown game-window operation.' }
  $game = Get-Process -Id $GameProcessId -ErrorAction Stop
  if ($game.HasExited -or $game.ProcessName -ne 'keeperfx-dx12' -or [IO.Path]::GetFullPath($game.Path) -ne [IO.Path]::GetFullPath($GameExecutable)) { throw 'Window target is not the launched DX12 game executable.' }
  Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class LiveGameWindow {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct MinMaxInfo { public Point Reserved, MaxSize, MaxPosition, MinTrack, MaxTrack; }
    [StructLayout(LayoutKind.Sequential)] public struct MonitorInfo { public uint Size; public Rect Monitor, Work; public uint Flags; }
    public delegate bool EnumCallback(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumCallback callback, IntPtr data);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, System.Text.StringBuilder name, int capacity);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] static extern int GetSystemMetricsForDpi(int index, uint dpi);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr parameter, ref MinMaxInfo info, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] static extern IntPtr GetWindowLong64(IntPtr window, int index);
    [DllImport("user32.dll", EntryPoint="GetWindowLongW")] static extern int GetWindowLong32(IntPtr window, int index);
    [DllImport("user32.dll")] static extern bool AdjustWindowRectEx(ref Rect rect, uint style, bool menu, uint extendedStyle);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr target, int x, int y, int width, int height, IntPtr source, int sourceX, int sourceY, uint operation);
    static uint Style(IntPtr window, int index) { return unchecked((uint)(IntPtr.Size==8?GetWindowLong64(window,index).ToInt64():GetWindowLong32(window,index))); }
    static int[] Dimensions(Point point) { return new int[] {point.X,point.Y}; }
    static int[] Coordinates(Rect rect) { return new int[] {rect.Left,rect.Top,rect.Right,rect.Bottom}; }
    public static Dictionary<string,object> Constraints(IntPtr window) {
        uint dpi=GetDpiForWindow(window);if(dpi==0)throw new InvalidOperationException("Owned window DPI query failed");
        var monitor=new MonitorInfo {Size=(uint)Marshal.SizeOf(typeof(MonitorInfo))};
        if(!GetMonitorInfo(MonitorFromWindow(window,2),ref monitor))throw new InvalidOperationException("Owned window monitor query failed");
        var info=new MinMaxInfo {
            MaxSize=new Point {X=GetSystemMetricsForDpi(61,dpi),Y=GetSystemMetricsForDpi(62,dpi)},
            MaxPosition=new Point {X=-GetSystemMetricsForDpi(32,dpi),Y=-GetSystemMetricsForDpi(33,dpi)},
            MinTrack=new Point {X=GetSystemMetricsForDpi(34,dpi),Y=GetSystemMetricsForDpi(35,dpi)},
            MaxTrack=new Point {X=GetSystemMetricsForDpi(59,dpi),Y=GetSystemMetricsForDpi(60,dpi)}
        };
        var evidence=new Dictionary<string,object> {
            {"dpi",dpi},{"monitor",Coordinates(monitor.Monitor)},{"workArea",Coordinates(monitor.Work)},
            {"seedSource","GetSystemMetricsForDpi defaults before the target WM_GETMINMAXINFO handler"},
            {"seedMinTrack",Dimensions(info.MinTrack)},{"seedMaxTrack",Dimensions(info.MaxTrack)},
            {"seedMaxSize",Dimensions(info.MaxSize)},{"seedMaxPosition",Dimensions(info.MaxPosition)}
        };
        IntPtr result;bool completed=SendMessageTimeout(window,0x24,IntPtr.Zero,ref info,2,500,out result)!=IntPtr.Zero;
        int error=completed?0:Marshal.GetLastWin32Error();
        evidence.Add("wmQuerySucceeded",completed);evidence.Add("wmQueryTimeoutMs",500);evidence.Add("wmQueryError",error);
        evidence.Add("wmQueryResult",result.ToInt64());
        evidence.Add("windowMinTrack",Dimensions(info.MinTrack));evidence.Add("windowMaxTrack",Dimensions(info.MaxTrack));
        evidence.Add("windowMaxSize",Dimensions(info.MaxSize));evidence.Add("windowMaxPosition",Dimensions(info.MaxPosition));
        return evidence;
    }
    public static IntPtr Find(uint process) {
        SetThreadDpiAwarenessContext(new IntPtr(-4));
        var windows=new List<IntPtr>();
        EnumWindows((window,data)=>{ uint owner; GetWindowThreadProcessId(window,out owner); if(owner==process) { var name=new System.Text.StringBuilder(128); GetClassName(window,name,128); if(name.ToString().StartsWith("SDL",StringComparison.Ordinal)) windows.Add(window); } return true; },IntPtr.Zero);
        if(windows.Count!=1)throw new InvalidOperationException("Expected exactly one SDL window belonging to the launched game");
        return windows[0];
    }
    public static void Resize(IntPtr window, int width, int height) {
        if(width<320||width>7680||height<240||height>4320)throw new ArgumentOutOfRangeException("Client size");
        if(IsZoomed(window)||IsIconic(window))ShowWindowAsync(window,9);
        Rect outer; if(!GetWindowRect(window,out outer))throw new InvalidOperationException("GetWindowRect failed");
        var rect=new Rect { Right=width,Bottom=height };
        if(!AdjustWindowRectEx(ref rect,Style(window,-16),false,Style(window,-20)))throw new InvalidOperationException("AdjustWindowRectEx failed");
        if(!SetWindowPos(window,IntPtr.Zero,outer.Left,outer.Top,rect.Right-rect.Left,rect.Bottom-rect.Top,0x14))throw new InvalidOperationException("Game client resize failed");
    }
    static bool Content(Bitmap bitmap) {
        var colors=new HashSet<int>();
        for(int y=1;y<10;y++)for(int x=1;x<10;x++)colors.Add(bitmap.GetPixel(bitmap.Width*x/10,bitmap.Height*y/10).ToArgb());
        return colors.Count>8;
    }
    public static string Capture(IntPtr window, string path, int width, int height) {
        if(IsIconic(window)||!IsWindowVisible(window))throw new InvalidOperationException("Cannot capture a minimized or hidden game client");
        using(var bitmap=new Bitmap(width,height,PixelFormat.Format32bppArgb))using(var graphics=Graphics.FromImage(bitmap)) {
            string method="PrintWindow-client";IntPtr target=graphics.GetHdc();
            try { if(!PrintWindow(window,target,3))method="WindowDC-client"; } finally { graphics.ReleaseHdc(target); }
            if(method=="WindowDC-client"||!Content(bitmap)) {
                method="WindowDC-client";target=graphics.GetHdc();IntPtr source=GetDC(window);
                if(source==IntPtr.Zero){graphics.ReleaseHdc(target);throw new InvalidOperationException("GetDC(game HWND) failed");}
                try { if(!BitBlt(target,0,0,width,height,source,0,0,0x00CC0020))throw new InvalidOperationException("Game client BitBlt failed"); }
                finally { ReleaseDC(window,source);graphics.ReleaseHdc(target); }
            }
            if(!Content(bitmap))throw new InvalidOperationException("Game client capture is blank or lacks visible world detail");
            bitmap.Save(path,ImageFormat.Png);return method;
        }
    }
}
'@
  $window = [LiveGameWindow]::Find([uint32]$GameProcessId)
  [uint32]$owner = 0
  [LiveGameWindow]::GetWindowThreadProcessId($window,[ref]$owner) | Out-Null
  if ($owner -ne $GameProcessId -or -not [LiveGameWindow]::IsWindow($window)) { throw 'Game HWND ownership changed.' }
  $result.windowHandle = $window.ToInt64().ToString()
  $result.operation = $request.operation
  $constraints = [LiveGameWindow]::Constraints($window)
  $result.constraints = $constraints
  $timer = [Diagnostics.Stopwatch]::StartNew()
  switch ($request.operation) {
    'resize' { [LiveGameWindow]::Resize($window,[int]$request.width,[int]$request.height) }
    'minimize' { [LiveGameWindow]::ShowWindowAsync($window,6) | Out-Null }
    'restore' {
      if (@($request.rect).Count -ne 4) { throw 'Restore requires the original outer window rectangle.' }
      $rect = @($request.rect | ForEach-Object { [int]$_ })
      if ($rect[2] -le $rect[0] -or $rect[3] -le $rect[1] -or $rect[2]-$rect[0] -gt 8000 -or $rect[3]-$rect[1] -gt 5000) { throw 'Invalid original window rectangle.' }
      # The asynchronous show event must finish before applying its new rect.
      # A normal window needs only SetWindowPos; another SW_RESTORE can apply
      # the saved normal placement after that size change.
      $result.restoreShowRequested = [LiveGameWindow]::IsIconic($window) -or [LiveGameWindow]::IsZoomed($window)
      if ($result.restoreShowRequested) {
        if (-not [LiveGameWindow]::ShowWindowAsync($window,9)) { throw 'Game restore event could not be queued.' }
        while (([LiveGameWindow]::IsIconic($window) -or [LiveGameWindow]::IsZoomed($window)) -and $timer.Elapsed.TotalSeconds -lt 3) { Start-Sleep -Milliseconds 25 }
        if ([LiveGameWindow]::IsIconic($window) -or [LiveGameWindow]::IsZoomed($window)) { throw 'Owned game window did not leave its minimized/maximized state within 3 seconds.' }
      }
      if (-not [LiveGameWindow]::SetWindowPos($window,[IntPtr]::Zero,$rect[0],$rect[1],$rect[2]-$rect[0],$rect[3]-$rect[1],0x14)) { throw 'Original game window rectangle could not be restored.' }
      if ($request.maximized) { [LiveGameWindow]::ShowWindowAsync($window,3) | Out-Null }
      if ($request.minimized) { [LiveGameWindow]::ShowWindowAsync($window,6) | Out-Null }
    }
  }
  do {
    $client = New-Object LiveGameWindow+Rect
    $outer = New-Object LiveGameWindow+Rect
    if (-not [LiveGameWindow]::GetClientRect($window,[ref]$client) -or -not [LiveGameWindow]::GetWindowRect($window,[ref]$outer)) { throw 'Game window disappeared during operation.' }
    $width = $client.Right-$client.Left; $height = $client.Bottom-$client.Top
    $minimized = [LiveGameWindow]::IsIconic($window)
    $result.clientSize = @($width,$height); $result.rect = @($outer.Left,$outer.Top,$outer.Right,$outer.Bottom)
    $result.minimized = $minimized; $result.maximized = [LiveGameWindow]::IsZoomed($window); $result.visible = [LiveGameWindow]::IsWindowVisible($window)
    $result.settleElapsedMs = [int]$timer.Elapsed.TotalMilliseconds
    $restoredRect = $request.operation -ne 'restore' -or $request.maximized -or ($outer.Left -eq $rect[0] -and $outer.Top -eq $rect[1] -and $outer.Right -eq $rect[2] -and $outer.Bottom -eq $rect[3])
    $allowedClamp = $false
    if ($request.operation -eq 'restore' -and -not $request.maximized -and -not $request.minimized -and -not $restoredRect -and $constraints.wmQuerySucceeded) {
      $limit = $constraints.windowMaxTrack
      $requestedWidth=$rect[2]-$rect[0]; $requestedHeight=$rect[3]-$rect[1]
      $appliedWidth=$outer.Right-$outer.Left; $appliedHeight=$outer.Bottom-$outer.Top
      if ($limit[0] -gt 0 -and $limit[1] -gt 0) {
        $allowedClamp = $outer.Left -eq $rect[0] -and $outer.Top -eq $rect[1] -and $appliedWidth -eq [Math]::Min($requestedWidth,$limit[0]) -and $appliedHeight -eq [Math]::Min($requestedHeight,$limit[1])
        if ($allowedClamp) { $result.clamp = @{source='returned WM_GETMINMAXINFO maximum tracking size'; limit=$limit; requestedOuterSize=@($requestedWidth,$requestedHeight); appliedOuterSize=@($appliedWidth,$appliedHeight); delta=@(($requestedWidth-$appliedWidth),($requestedHeight-$appliedHeight))} }
      }
    }
    $settled = ($request.operation -ne 'resize' -or ($width -eq $request.width -and $height -eq $request.height)) -and ($request.operation -ne 'minimize' -or $minimized) -and ($request.operation -ne 'restore' -or ($minimized -eq [bool]$request.minimized -and [LiveGameWindow]::IsZoomed($window) -eq [bool]$request.maximized -and ($restoredRect -or $allowedClamp)))
    $result.settled = $settled
    if ($request.operation -eq 'restore') { $result.requestedRect = $rect; $result.rectMatches = $restoredRect; $result.allowedClamp = $allowedClamp; $result.requestedMinimized = [bool]$request.minimized; $result.requestedMaximized = [bool]$request.maximized }
    if ($settled) { break }
    Start-Sleep -Milliseconds 25
  } while ($timer.Elapsed.TotalSeconds -lt 3)
  if (-not $settled) { throw 'Owned game window operation did not settle within 3 seconds.' }
  $result.clientSize = @($width,$height); $result.rect = @($outer.Left,$outer.Top,$outer.Right,$outer.Bottom)
  $result.minimized = $minimized; $result.maximized = [LiveGameWindow]::IsZoomed($window); $result.visible = [LiveGameWindow]::IsWindowVisible($window)
  if ($request.operation -eq 'capture') {
    if ($request.image -notmatch '^[a-z0-9-]+\.png$') { throw 'Capture image must be a simple PNG filename in the evidence directory.' }
    $imagePath = Join-Path $requestDirectory $request.image
    $result.captureMethod = [LiveGameWindow]::Capture($window,$imagePath,$width,$height)
    $result.image = $request.image
    $bitmap = New-Object Drawing.Bitmap $imagePath
    try {
      $result.pixels = @($request.points | ForEach-Object {
        $x=[int]$_.x; $y=[int]$_.y
        if ($x -lt 0 -or $y -lt 0 -or $x -ge $width -or $y -ge $height) { throw 'Client sample lies outside the owned window.' }
        $pixel=$bitmap.GetPixel($x,$y)
        @{x=$x;y=$y;rgba=@([int]$pixel.R,[int]$pixel.G,[int]$pixel.B,[int]$pixel.A)}
      })
    } finally { $bitmap.Dispose() }
  }
  $result.passed = $true
} catch {
  $result.error=$_.Exception.Message
  # Keep the final owned-window observation even when an earlier restore phase
  # failed before the normal settle loop could publish it.
  if ($window -and [LiveGameWindow]::IsWindow($window)) {
    $client = New-Object LiveGameWindow+Rect; $outer = New-Object LiveGameWindow+Rect
    if ([LiveGameWindow]::GetClientRect($window,[ref]$client) -and [LiveGameWindow]::GetWindowRect($window,[ref]$outer)) {
      $result.clientSize = @(($client.Right-$client.Left),($client.Bottom-$client.Top)); $result.rect = @($outer.Left,$outer.Top,$outer.Right,$outer.Bottom)
      $result.minimized = [LiveGameWindow]::IsIconic($window); $result.maximized = [LiveGameWindow]::IsZoomed($window); $result.visible = [LiveGameWindow]::IsWindowVisible($window)
    }
  }
}
[IO.File]::WriteAllText($ResultPath,($result | ConvertTo-Json -Depth 5))
if (-not $result.passed) { throw $result.error }
