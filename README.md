ldrshrink
=========

This tool is a possible aid for engineers writing low-level boot code for the following processors who are trying to eek out the fastest boot time.

Analog Devices has a bevy of products:

[ADSP-SC58x](http://www.analog.com/en/products/landing-pages/001/adsp-sc58x-adsp-2158x-series.html)

[ADSP-SC57x](http://www.analog.com/en/products/landing-pages/001/adsp-sc57x-2157x-family.html)

[ADSP-BF70x](http://www.analog.com/en/products/landing-pages/001/adsp-bf70xseries.html)

that all boot from a specially crafted "boot loader stream" image.

Said loader image is created by converting a .dxe (ELF file that Analog gives a different name) using an elfloader.exe tool.  (Earlier Blackfin processors also had a similar procedure and an earlier version of the same tool, but used a different loader image format.)

The output of elfloader.exe is not as optimized as it could be, and neither is the Boot ROM in the processors.  These two limitations conspire to make booting of small bootloader slower than it could be.

Unfortunately, we are stuck with the limitations in the Boot ROM.  However, post-processing of the loader file (beyond the effort made by elfloader.exe) can yield some boot time improvements.

I provided Analog an earlier version of this code in Nov 2015.  I got a nice email back from them (much nicer than most tech support emails from semiconductor companies these days), but was told that it wasn't a priority to improve.

I am making the code available in solidarity with other engineers who might find themselves in the same situation as I did.

## Usage

```
ldrshrink original.ldr possiblyimproved.ldr
```

## Limitations

The tool was written for single core loader images, as this is the sweet spot for small boot times.  Quite frankly, if you are relying on the Boot ROM to quickly boot a multi-core image (SC5xx), expect to be disappointed.  In my opinion, it is better for the master processor to boot ASAP first and have it drive an application-optimized boot of additional cores.

