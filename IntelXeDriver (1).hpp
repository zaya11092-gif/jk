/*
 * IntelXeDriver.hpp
 * Phase 1 — Low-level kernel driver stub for Intel Gen 12 Iris Xe Graphics
 *
 * Target hardware  : Alder Lake-P Integrated Graphics (Device ID 0x46A6)
 * Target arch      : x86_64  (XNU kernel, I/O Kit)
 * Language subset  : Kernel-safe C++  — no exceptions, no RTTI, no STL
 */

#ifndef INTEL_XE_DRIVER_HPP
#define INTEL_XE_DRIVER_HPP

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

/* ------------------------------------------------------------------ */
/*  PCI / hardware constants                                          */
/* ------------------------------------------------------------------ */

#define INTEL_VENDOR_ID          0x8086u
#define ADLERLAKE_XE_DID         0x46A6u

/* ------------------------------------------------------------------ */
/*  Forced default display mode (1920 x 1080 x 32 bpp)               */
/* ------------------------------------------------------------------ */

#define XE_DEFAULT_WIDTH         1920u
#define XE_DEFAULT_HEIGHT        1080u
#define XE_DEFAULT_BPP           32u
#define XE_DEFAULT_REFRESH_HZ    60u

/* Display-mode ID we export (single entry) */
#define XE_DISPLAY_MODE_1080P    0

/* ------------------------------------------------------------------ */
/*  IntelXeDriver — IOFramebuffer subclass                           */
/* ------------------------------------------------------------------ */

class IntelXeDriver : public IOFramebuffer
{
    OSDeclareDefaultStructors(IntelXeDriver);

public:

    /* --- IOService lifecycle ---------------------------------------- */

    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider)  APPLE_KEXT_OVERRIDE;

    /* --- IOFramebuffer: display-mode enumeration ------------------- */

    virtual IOItemCount getDisplayModeCount(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn    getDisplayModes(
                            IODisplayModeID *allDisplayModes) APPLE_KEXT_OVERRIDE;
    virtual IOReturn    getDisplayModeInformation(
                            IODisplayModeID           displayMode,
                            IODisplayModeInformation  *info) APPLE_KEXT_OVERRIDE;

    /* --- IOFramebuffer: pixel / depth queries ---------------------- */

    virtual IOReturn    getPixelInformation(
                            IODisplayModeID   displayMode,
                            IOIndex           depth,
                            IOPixelInformation *pixelInfo) APPLE_KEXT_OVERRIDE;
    virtual IOReturn    getCurrentDisplayMode(
                            IODisplayModeID  *displayMode,
                            IOIndex          *depth) APPLE_KEXT_OVERRIDE;
    virtual IOReturn    setDisplayMode(
                            IODisplayModeID  displayMode,
                            IOIndex          depth) APPLE_KEXT_OVERRIDE;

    /* --- IOFramebuffer: aperture (framebuffer memory window) ------- */

    virtual IOReturn    setApertureEnable(
                            IOIndex       aperture,
                            IOOptionBits  enable) APPLE_KEXT_OVERRIDE;

    /* --- IOFramebuffer: cursor (stubs — no HW cursor in Phase 1) -- */

    virtual bool        setCursorVisible(bool visible) APPLE_KEXT_OVERRIDE;
    virtual bool        setCursorState(
                            IOGPoint     *hotSpot,
                            int          *frame,
                            IOOptionBits  state) APPLE_KEXT_OVERRIDE;
    virtual void        flushCursor(void) APPLE_KEXT_OVERRIDE;

    /* --- IOFramebuffer: gamma (stub — linear pass-through) --------- */

    virtual IOReturn    setGammaTable(
                            IOIndex       channel,
                            IOItemCount   count,
                            IOGammaEntry  *data) APPLE_KEXT_OVERRIDE;

private:

    /* PCI provider reference */
    IOPCIDevice                  *fPCIDevice;

    /* MMIO register window mapped from BAR 0 (GTTMMADR) */
    IOMemoryMap                  *fMMIOMap;
    volatile uint8_t             *fMMIOBase;

    /* Framebuffer backing store */
    IOBufferMemoryDescriptor     *fFramebufferDesc;
    void                         *fFramebufferMemory;
    uint64_t                      fFramebufferPhysical;
    uint32_t                      fFramebufferSize;

    /* Active display mode bookkeeping */
    IODisplayModeID               fCurrentMode;
    IOIndex                       fCurrentDepth;

    /* --- Internal helpers ------------------------------------------ */

    bool  initHardware(IOService *provider);
    void  unmapPCI(void);
    bool  allocateFramebuffer(void);
    void  releaseFramebuffer(void);
};

#endif  /* !INTEL_XE_DRIVER_HPP */