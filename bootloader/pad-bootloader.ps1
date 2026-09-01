param(
    [string]$Path = "bootloader.bin",
    [int]$Size = 0x8000
)

$image = [System.IO.File]::ReadAllBytes($Path)
if ($image.Length -gt $Size) {
    throw "Bootloader image is larger than the reserved 32 KiB region."
}

$paddedImage = New-Object byte[] $Size
for ($index = 0; $index -lt $Size; ++$index) {
    $paddedImage[$index] = 0xFF
}
[Array]::Copy($image, $paddedImage, $image.Length)
[System.IO.File]::WriteAllBytes($Path, $paddedImage)
