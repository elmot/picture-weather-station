for %%f in (svg\*.svg) do (
	"C:\Program Files\Inkscape\bin\inkscape.exe" "%%f" --export-type=png --export-filename="..\pict\svg_png\%%~nf.png" --export-width=80 --export-height=80 --export-background-opacity=1.0 --export-background=#FF7F00
)