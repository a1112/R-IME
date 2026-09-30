# Render the vector geometry from r-ime.svg into multi-resolution Windows icons.
# System.Drawing is used only to generate repository assets; it is not a runtime dependency.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
foreach($variant in @(@{Name='r-ime';Color='#3478f6'},@{Name='r-ime-ascii';Color='#687487'})){
    $images=[Collections.Generic.List[object]]::new()
    foreach($size in @(16,20,24,32,40,48,64,128,256)){
        $large=[Drawing.Bitmap]::new($size*4,$size*4)
        $graphics=[Drawing.Graphics]::FromImage($large)
        $graphics.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.ScaleTransform($size/16.0,$size/16.0)
        $box=[Drawing.Drawing2D.GraphicsPath]::new()
        $box.AddArc(3,3,30,30,180,90);$box.AddArc(31,3,30,30,270,90)
        $box.AddArc(31,31,30,30,0,90);$box.AddArc(3,31,30,30,90,90);$box.CloseFigure()
        $brush=[Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($variant.Color))
        $graphics.FillPath($brush,$box)
        $letter=[Drawing.Drawing2D.GraphicsPath]::new()
        $letter.AddLine(21,47,21,17);$letter.AddLine(21,17,33,17)
        $letter.AddBezier(33,17,41,17,45,21,45,27)
        $letter.AddBezier(45,27,45,32,42,35,38,36)
        $letter.AddLine(38,36,47,47);$letter.AddLine(47,47,38,47)
        $letter.AddLine(38,47,27,33);$letter.AddLine(27,33,33,33)
        $letter.AddBezier(33,33,37,33,39,31,39,27)
        $letter.AddBezier(39,27,39,23,37,22,33,22)
        $letter.AddLine(33,22,27,22);$letter.AddLine(27,22,27,47);$letter.CloseFigure()
        $graphics.FillPath([Drawing.Brushes]::White,$letter)
        $small=[Drawing.Bitmap]::new($size,$size)
        $scaled=[Drawing.Graphics]::FromImage($small)
        $scaled.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $scaled.DrawImage($large,0,0,$size,$size)
        $png=[IO.MemoryStream]::new();$small.Save($png,[Drawing.Imaging.ImageFormat]::Png)
        $images.Add(@{Size=$size;Data=$png.ToArray()})
        if($size -eq 128){$small.Save((Join-Path $PSScriptRoot ($variant.Name+'.png')),[Drawing.Imaging.ImageFormat]::Png)}
        $png.Dispose();$scaled.Dispose();$small.Dispose();$letter.Dispose();$brush.Dispose();$box.Dispose();$graphics.Dispose();$large.Dispose()
    }
    $stream=[IO.File]::Create((Join-Path $PSScriptRoot ($variant.Name+'.ico')))
    $writer=[IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]$images.Count)
        $offset=6+16*$images.Count
        foreach($entry in $images){
            $dimension=if($entry.Size -eq 256){0}else{$entry.Size}
            $writer.Write([byte]$dimension);$writer.Write([byte]$dimension)
            $writer.Write([byte]0);$writer.Write([byte]0);$writer.Write([uint16]1);$writer.Write([uint16]32)
            $writer.Write([uint32]$entry.Data.Length);$writer.Write([uint32]$offset);$offset+=$entry.Data.Length
        }
        foreach($entry in $images){$writer.Write([byte[]]$entry.Data)}
    } finally {$writer.Dispose();$stream.Dispose()}
}
