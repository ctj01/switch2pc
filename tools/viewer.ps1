# Live viewer for hosrun on Windows: shows the BMP that HOS_FRAMES_LIVE points
# at (reloaded at ~30 Hz) and writes the keyboard state to the file that
# HOS_INPUT_FILE points at, so a game running under WSL/qemu is playable.
#
#   powershell -ExecutionPolicy Bypass -File tools\viewer.ps1 <frame.bmp> <input.txt> [scale]
#
# Keys: arrows = d-pad, Z/Ctrl = A, X/Space = B, C = X, V = Y, Q = L, E = R,
#       Enter = Plus, Esc = Minus. Closing the window tells hosrun to quit.
#
# The keyboard is polled with GetAsyncKeyState while this window is in the
# foreground; WinForms key events are not used because arrow keys are
# navigation keys there and arrive unreliably. The title bar shows the pad
# mask being sent, which is the first thing to look at when input seems dead.
param(
    [Parameter(Mandatory = $true)][string]$Frame,
    [Parameter(Mandatory = $true)][string]$PadFile,
    [int]$Scale = 2,
    [switch]$SelfTest
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace S2pc -Name Native -MemberDefinition @"
[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
"@

# Virtual-key code -> Switch button bit (HidNpadButton numbering).
$keymap = @(
    @{ vk = 0x5A; bit = 0 },  # Z      -> A
    @{ vk = 0x11; bit = 0 },  # Ctrl   -> A
    @{ vk = 0x58; bit = 1 },  # X      -> B
    @{ vk = 0x20; bit = 1 },  # Space  -> B
    @{ vk = 0x43; bit = 2 },  # C      -> X
    @{ vk = 0x56; bit = 3 },  # V      -> Y
    @{ vk = 0x51; bit = 6 },  # Q      -> L
    @{ vk = 0x45; bit = 7 },  # E      -> R
    @{ vk = 0x0D; bit = 10 }, # Enter  -> Plus
    @{ vk = 0x1B; bit = 11 }, # Esc    -> Minus
    @{ vk = 0x25; bit = 12 }, # Left
    @{ vk = 0x26; bit = 13 }, # Up
    @{ vk = 0x27; bit = 14 }, # Right
    @{ vk = 0x28; bit = 15 }  # Down
)

function Read-Pad {
    $mask = [uint64]0
    foreach ($k in $keymap) {
        if (([S2pc.Native]::GetAsyncKeyState($k.vk) -band 0x8000) -ne 0) {
            $mask = $mask -bor ([uint64]1 -shl $k.bit)
        }
    }
    return $mask
}

function Write-Pad([uint64]$mask) {
    $text = ("{0:x} 0 0 0 0" -f $mask)
    # hosrun may be reading the file through WSL at this instant; retry briefly.
    for ($try = 0; $try -lt 20; $try++) {
        try {
            [System.IO.File]::WriteAllText($PadFile, $text)
            return
        } catch {
            Start-Sleep -Milliseconds 2
        }
    }
}

if ($SelfTest) {
    "pad mask now: {0:x}" -f (Read-Pad)
    Write-Pad 0
    "wrote: " + (Get-Content $PadFile)
    exit 0
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "switch2pc"
$form.FormBorderStyle = "FixedSingle"
$form.MaximizeBox = $false
$form.BackColor = [System.Drawing.Color]::Black

$box = New-Object System.Windows.Forms.PictureBox
$box.SizeMode = "StretchImage"
$box.Dock = "Fill"
$form.Controls.Add($box)
$form.ClientSize = New-Object System.Drawing.Size(640 * $Scale, 400 * $Scale)

$form.Add_FormClosing({
    [System.IO.File]::WriteAllText($PadFile, "quit")
})

$script:lastWrite = [DateTime]::MinValue
$script:frames = 0
$script:lastMask = [uint64]0xFFFFFFFF

$padTimer = New-Object System.Windows.Forms.Timer
$padTimer.Interval = 16
$padTimer.Add_Tick({
    $mask = [uint64]0
    if ([S2pc.Native]::GetForegroundWindow() -eq $form.Handle) {
        $mask = Read-Pad
    }
    if ($mask -ne $script:lastMask) {
        $script:lastMask = $mask
        Write-Pad $mask
        $form.Text = ("switch2pc  frame {0}  pad {1:x}" -f $script:frames, $mask)
    }
})

$frameTimer = New-Object System.Windows.Forms.Timer
$frameTimer.Interval = 33
$frameTimer.Add_Tick({
    if (-not (Test-Path $Frame)) { return }
    $stamp = (Get-Item $Frame).LastWriteTimeUtc
    if ($stamp -eq $script:lastWrite) { return }
    $script:lastWrite = $stamp
    try {
        $bytes = [System.IO.File]::ReadAllBytes($Frame)
        $stream = New-Object System.IO.MemoryStream(, $bytes)
        $img = [System.Drawing.Image]::FromStream($stream)
        $old = $box.Image
        $box.Image = $img
        if ($old) { $old.Dispose() }
        $want = New-Object System.Drawing.Size($img.Width * $Scale, $img.Height * $Scale)
        if ($form.ClientSize -ne $want) { $form.ClientSize = $want }
        $script:frames++
        if (($script:frames % 30) -eq 0) {
            $form.Text = ("switch2pc  frame {0}  pad {1:x}" -f $script:frames, $script:lastMask)
        }
    } catch {
        # a frame being replaced; try again on the next tick
    }
})

$padTimer.Start()
$frameTimer.Start()
Write-Pad 0
[void]$form.ShowDialog()
