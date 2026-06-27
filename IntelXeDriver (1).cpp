/*
 * IntelXeDriver.cpp
 * Phase 1 — Low-level kernel driver stub for Intel Gen 12 Iris Xe Graphics
 *
 * Target hardware  : Alder Lake-P Integrated Graphics (Device ID 0x46A6)
 * Target arch      : x86_64  (XNU kernel, I/O Kit)
 * Language subset  : Kernel-safe C++  — no exceptions, no RTTI, no STL
 *
 * What Phase 1 does:
 *   1. Matches Intel 8086:46A6 via IOKitPersonalities / IOPCIPrimaryMatch.
 *   2. Maps PCI BAR 0 (GTTMMADR) for MMIO register access and probes a
 *      handful of registers to verify the mapping is functional.
 *   3. Allocates a physically-contiguous framebuffer (1920x1080x32 bpp)
 *      via IOBufferMemoryDescriptor and registers it as aperture 0.
 *   4. Exports a single forced 1080p display mode to the IOFramebuffer
 *      base class so the upper display layers (WindowServer) can attach.
 */

#include "IntelXeDriver.hpp"

/* ================================================================== */
/*  Meta-class / structor registration                                */
/* ================================================================== */

#define super IOFramebuffer

OSDefineMetaClassAndStructors(IntelXeDriver, IOFramebuffer);

/* ================================================================== */
/*  IOService lifecycle                                                */
/* ================================================================== */

/*
 * start
 *
 * Called by IOKit after a successful personality match on IOPCIDevice
 * with IOPCIPrimaryMatch = 0x808646A6.
 *
 * Steps:
 *   1.  super::start() — IOFramebuffer base-class setup.
 *   2.  Validate the provider is an IOPCIDevice and open it.
 *   3.  Enable PCI bus-master + memory-space decoding.
 *   4.  initHardware() — map BAR 0, verify PCI & MMIO registers.
 *   5.  allocateFramebuffer() — phys-contiguous 1080p surface.
 *   6.  Set the default mode and publish the service.
 */
bool
IntelXeDriver::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    /* ---- Validate provider type ---------------------------------- */
    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        IOLog("IntelXeDriver: provider is not an IOPCIDevice\n");
        goto fail_early;
    }

    /* ---- Open the PCI device exclusively -------------------------- */
    if (!fPCIDevice->open(this)) {
        IOLog("IntelXeDriver: failed to open PCI device\n");
        goto fail_early;
    }

    /* ---- Enable bus-master and memory-space decoding -------------- */
    fPCIDevice->setBusMasterEnable(true);
    fPCIDevice->setMemoryEnable(true);

    /* ---- Map BAR 0 and probe hardware ----------------------------- */
    if (!initHardware(provider)) {
        IOLog("IntelXeDriver: hardware initialisation failed\n");
        goto fail_close;
    }

    /* ---- Allocate the in-memory framebuffer surface --------------- */
    if (!allocateFramebuffer()) {
        IOLog("IntelXeDriver: framebuffer allocation failed\n");
        goto fail_unmap;
    }

    /* ---- Initialise to the forced 1080p mode ---------------------- */
    fCurrentMode  = XE_DISPLAY_MODE_1080P;
    fCurrentDepth = 0;

    IOLog("IntelXeDriver: started  vendor=0x%04X  device=0x%04X  "
          "fb phys=0x%llX  fb size=0x%X\n",
          INTEL_VENDOR_ID, ADLERLAKE_XE_DID,
          fFramebufferPhysical, fFramebufferSize);

    registerService(kIOServiceAsynchronous);
    return true;

    /* ---- Cleanup ladder (reverse order of allocation) ------------ */
fail_unmap:
    unmapPCI();
fail_close:
    fPCIDevice->close(this);
fail_early:
    super::stop(provider);
    return false;
}

/*
 * stop
 *
 * Called by IOKit during driver teardown (kextunload or terminate).
 * Every resource acquired in start() must be released here.
 */
void
IntelXeDriver::stop(IOService *provider)
{
    releaseFramebuffer();
    unmapPCI();

    if (fPCIDevice) {
        fPCIDevice->close(this);
        fPCIDevice = NULL;
    }

    IOLog("IntelXeDriver: stopped\n");
    super::stop(provider);
}

/* ================================================================== */
/*  Hardware initialisation — BAR mapping & register verification      */
/* ================================================================== */

/*
 * initHardware
 *
 * Maps PCI BAR 0 (the GTTMMADR register aperture on Intel GPUs) into
 * kernel virtual address space and performs a handful of register reads
 * to confirm the mapping is live and the device is responsive.
 *
 * On Gen 12 the BAR 0 region contains the GTTMMADR base:
 *   - Lower half  = MMIO register space  (engine registers, display regs)
 *   - Upper half  = Global GTT           (Graphics Translation Table)
 *
 * For Phase 1 we only need to prove we can read from the MMIO half.
 */
bool
IntelXeDriver::initHardware(IOService *provider)
{
    provider;   /* satisfy unused-parameter warning in release builds */

    /* ---- Map BAR 0 into the kernel address space ------------------ */
    fMMIOMap = fPCIDevice->mapDeviceMemoryWithRegister(
                   kIOPCIConfigBaseAddress0,   /* BAR index */
                   kIOMapAnywhere);            /* let IOKit pick the VA */

    if (!fMMIOMap) {
        IOLog("IntelXeDriver: failed to map PCI BAR 0 (GTTMMADR)\n");
        return false;
    }

    fMMIOBase = (volatile uint8_t *)fMMIOMap->getVirtualAddress();

    /* ---- Read PCI configuration space for verification ------------ */
    uint32_t vendorDevice = fPCIDevice->configRead32(kIOPCIConfigVendorID);
    uint16_t vendorID     = (uint16_t)(vendorDevice        & 0xFFFFu);
    uint16_t deviceID     = (uint16_t)((vendorDevice >> 16) & 0xFFFFu);

    uint16_t pciCmd = fPCIDevice->configRead16(kIOPCIConfigCommand);
    uint8_t  revID  = fPCIDevice->configRead8(kIOPCIConfigRevisionID);
    uint8_t  progIF = fPCIDevice->configRead8(kIOPCIConfigClassCode + 1);

    IOLog("IntelXeDriver: PCI  %04X:%04X  rev=0x%02X  progIF=0x%02X  "
          "cmd=0x%04X\n", vendorID, deviceID, revID, progIF, pciCmd);
    IOLog("IntelXeDriver: BAR0 mapped  va=%p  length=0x%llX\n",
          fMMIOBase, (uint64_t)fMMIOMap->getLength());

    /* Sanity-check: did we match the device we expected? */
    if (vendorID != INTEL_VENDOR_ID || deviceID != ADLERLAKE_XE_DID) {
        IOLog("IntelXeDriver: PCI ID mismatch  "
              "(got %04X:%04X, expected %04X:%04X)\n",
              vendorID, deviceID, INTEL_VENDOR_ID, ADLERLAKE_XE_DID);
        return false;
    }

    /* ---- Probe a few MMIO offsets to verify register access ------- */
    /*
     * NOTE: On Gen 12+, offset 0x0000 reads the first register in the
     * MMIO space.  The exact functional meaning is hardware-specific and
     * will be decoded properly in Phase 2+.  Here we only care that the
     * read completes without a machine check.
     */
    uint32_t mmio_0000 = OSReadLittleInt32(fMMIOBase, 0x0000);
    uint32_t mmio_0004 = OSReadLittleInt32(fMMIOBase, 0x0004);
    uint32_t mmio_D00  = OSReadLittleInt32(fMMIOBase, 0x0D00);
    uint32_t mmio_2000 = OSReadLittleInt32(fMMIOBase, 0x2000);

    IOLog("IntelXeDriver: MMIO probe  "
          "[0x0000]=0x%08X  [0x0004]=0x%08X  "
          "[0x0D00]=0x%08X  [0x2000]=0x%08X\n",
          mmio_0000, mmio_0004, mmio_D00, mmio_2000);

    return true;
}

/* ------------------------------------------------------------------ */
/*  PCI BAR cleanup                                                   */
/* ------------------------------------------------------------------ */

void
IntelXeDriver::unmapPCI(void)
{
    if (fMMIOMap) {
        fMMIOMap->release();
        fMMIOMap = NULL;
    }
    fMMIOBase = NULL;
}

/* ================================================================== */
/*  Framebuffer memory management                                      */
/* ================================================================== */

/*
 * allocateFramebuffer
 *
 * Creates a physically contiguous, non-pageable memory buffer large
 * enough for 1920 x 1080 x 4 bytes  (8 294 400 bytes ≈ 7.9 MiB) using
 * IOBufferMemoryDescriptor — the modern, officially supported
 * replacement for the deprecated IOMallocContiguous().
 *
 * The physical address is resolved immediately so it can be handed to
 * the IOFramebuffer base class via setAperture() later.
 */
bool
IntelXeDriver::allocateFramebuffer(void)
{
    fFramebufferSize = XE_DEFAULT_WIDTH *
                       XE_DEFAULT_HEIGHT *
                       (XE_DEFAULT_BPP / 8u);

    fFramebufferDesc = IOBufferMemoryDescriptor::inTaskWithOptions(
                           kernel_task,
                           kIOMemoryPhysicallyContiguous,
                           fFramebufferSize,
                           PAGE_SIZE);

    if (!fFramebufferDesc) {
        IOLog("IntelXeDriver: IOBufferMemoryDescriptor alloc failed "
              "(%u bytes, contiguous)\n", fFramebufferSize);
        return false;
    }

    fFramebufferMemory = fFramebufferDesc->getBytesNoCopy();

    /* Resolve the physical base address for aperture registration */
    addr64_t phys = fFramebufferDesc->getPhysicalSegment(0, NULL);
    if (phys == 0) {
        IOLog("IntelXeDriver: failed to resolve framebuffer physical "
              "address\n");
        fFramebufferDesc->release();
        fFramebufferDesc     = NULL;
        fFramebufferMemory   = NULL;
        return false;
    }
    fFramebufferPhysical = phys;

    /* Zero-fill so the display starts black (not random garbage) */
    bzero(fFramebufferMemory, fFramebufferSize);

    IOLog("IntelXeDriver: framebuffer allocated  virt=%p  "
          "phys=0x%llX  bytes=%u\n",
          fFramebufferMemory, fFramebufferPhysical, fFramebufferSize);

    return true;
}

/*
 * releaseFramebuffer
 *
 * Releases the IOBufferMemoryDescriptor, which automatically unmaps
 * any kernel mappings and frees the physical pages.
 */
void
IntelXeDriver::releaseFramebuffer(void)
{
    if (fFramebufferDesc) {
        fFramebufferDesc->release();
        fFramebufferDesc       = NULL;
    }
    fFramebufferMemory   = NULL;
    fFramebufferPhysical = 0;
    fFramebufferSize     = 0;
}

/* ================================================================== */
/*  IOFramebuffer overrides — display mode enumeration                */
/* ================================================================== */

IOItemCount
IntelXeDriver::getDisplayModeCount(void)
{
    return 1;   /* Only the forced 1080p mode for Phase 1 */
}

IOReturn
IntelXeDriver::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    if (!allDisplayModes)
        return kIOReturnBadArgument;

    allDisplayModes[0] = XE_DISPLAY_MODE_1080P;
    return kIOReturnSuccess;
}

IOReturn
IntelXeDriver::getDisplayModeInformation(
    IODisplayModeID              displayMode,
    IODisplayModeInformation    *info)
{
    if (!info)
        return kIOReturnBadArgument;

    if (displayMode != XE_DISPLAY_MODE_1080P)
        return kIOReturnUnsupportedMode;

    bzero(info, sizeof(*info));

    info->nominalWidth  = XE_DEFAULT_WIDTH;
    info->nominalHeight = XE_DEFAULT_HEIGHT;
    info->refreshRate   = (XE_DEFAULT_REFRESH_HZ << 16);  /* 16.16 fxpt */
    info->maxDepthIndex = 0;
    info->flags         = kIODisplayModeValidFlag | kIODisplayModeSafeFlag;

    return kIOReturnSuccess;
}

/* ================================================================== */
/*  IOFramebuffer overrides — pixel format queries                    */
/* ================================================================== */

IOReturn
IntelXeDriver::getPixelInformation(
    IODisplayModeID    displayMode,
    IOIndex            depth,
    IOPixelInformation *pixelInfo)
{
    if (!pixelInfo)
        return kIOReturnBadArgument;

    if (displayMode != XE_DISPLAY_MODE_1080P || depth != 0)
        return kIOReturnUnsupportedMode;

    bzero(pixelInfo, sizeof(*pixelInfo));

    pixelInfo->activeWidth    = XE_DEFAULT_WIDTH;
    pixelInfo->activeHeight   = XE_DEFAULT_HEIGHT;
    pixelInfo->bytesPerRow    = XE_DEFAULT_WIDTH * (XE_DEFAULT_BPP / 8u);
    pixelInfo->pixelType      = kIO30bitDirectPixels;
    pixelInfo->pixelFormat    = kIO32BitDirectPixels;
    pixelInfo->bitsPerPixel   = XE_DEFAULT_BPP;
    pixelInfo->componentCount = 3;

    /*
     * Component masks — ARGB logical layout.
     * x86_64 is little-endian, so in memory the byte order is B-G-R-A.
     */
    pixelInfo->componentMasks[0] = 0x00FF0000u;  /* Red   */
    pixelInfo->componentMasks[1] = 0x0000FF00u;  /* Green */
    pixelInfo->componentMasks[2] = 0x000000FFu;  /* Blue  */
    pixelInfo->componentMasks[3] = 0xFF000000u;  /* Alpha */

    return kIOReturnSuccess;
}

/* ================================================================== */
/*  IOFramebuffer overrides — mode get / set                          */
/* ================================================================== */

IOReturn
IntelXeDriver::getCurrentDisplayMode(IODisplayModeID *displayMode,
                                     IOIndex         *depth)
{
    if (displayMode)
        *displayMode = fCurrentMode;
    if (depth)
        *depth = fCurrentDepth;
    return kIOReturnSuccess;
}

IOReturn
IntelXeDriver::setDisplayMode(IODisplayModeID displayMode,
                              IOIndex         depth)
{
    if (displayMode != XE_DISPLAY_MODE_1080P || depth != 0)
        return kIOReturnUnsupportedMode;

    fCurrentMode  = displayMode;
    fCurrentDepth = depth;

    /*
     * Phase 2+ will program the actual display pipeline here:
     *   - Configure pipe/plane registers in MMIO
     *   - Set DPLL, DDI, transcoder timing
     *   - Point the plane surface to our framebuffer physical address
     */
    IOLog("IntelXeDriver: setDisplayMode  mode=%u  depth=%u  "
          "(HW programming deferred to Phase 2)\n",
          (unsigned)displayMode, (unsigned)depth);

    return kIOReturnSuccess;
}

/* ================================================================== */
/*  IOFramebuffer overrides — aperture (framebuffer memory window)    */
/* ================================================================== */

/*
 * setApertureEnable
 *
 * The IOFramebuffer base class calls this when the WindowServer wants
 * to map (or unmap) a region of framebuffer memory into user space.
 * Aperture 0 is the primary display surface.
 *
 * We satisfy the request by calling the inherited IODisplay::setAperture()
 * with the physical address and size of our IOBufferMemoryDescriptor.
 * The base class then creates the user-space mapping on our behalf.
 */
IOReturn
IntelXeDriver::setApertureEnable(IOIndex      aperture,
                                 IOOptionBits  enable)
{
    if (aperture != 0)
        return kIOReturnSuccess;   /* Only aperture 0 is valid here */

    if (enable & kIOApertureEnable) {
        if (fFramebufferPhysical != 0) {
            /*
             * setAperture() is a protected method inherited from
             * IODisplay.  It registers a physical memory range so
             * the IOFramebuffer base class can build a user-space
             * mapping for the CoreGraphics / WindowServer layer.
             *
             * Signature:  void setAperture(IOIndex, IOPhysicalAddress,
             *                                IOByteCount);
             */
            setAperture(aperture, (IOPhysicalAddress)fFramebufferPhysical,
                        (IOByteCount)fFramebufferSize);

            IOLog("IntelXeDriver: aperture 0 enabled  "
                  "phys=0x%llX  size=0x%X\n",
                  fFramebufferPhysical, fFramebufferSize);
        }
    } else {
        /* Tear-down: register a zero-length aperture */
        setAperture(aperture, 0, 0);
    }

    return kIOReturnSuccess;
}

/* ================================================================== */
/*  IOFramebuffer overrides — cursor (Phase 1 stubs, no HW cursor)    */
/* ================================================================== */

bool
IntelXeDriver::setCursorVisible(bool visible)
{
    /* Phase 1: no hardware cursor engine programmed */
    visible;   /* suppress unused-parameter warning */
    return true;
}

bool
IntelXeDriver::setCursorState(IOGPoint     *hotSpot,
                               int          *frame,
                               IOOptionBits  state)
{
    /* Phase 1 stub — hardware cursor overlay not yet wired */
    hotSpot;
    frame;
    state;
    return true;
}

void
IntelXeDriver::flushCursor(void)
{
    /* Phase 1 stub */
}

/* ================================================================== */
/*  IOFramebuffer overrides — gamma (Phase 1 stub, linear)            */
/* ================================================================== */

IOReturn
IntelXeDriver::setGammaTable(IOIndex       channel,
                              IOItemCount   count,
                              IOGammaEntry  *data)
{
    /*
     * Phase 1: accept but ignore gamma corrections.
     * The framebuffer will display with a linear (identity) LUT.
     * Phase 2+ will wire the PAL/GAMMA MMIO registers.
     */
    channel;
    count;
    data;
    return kIOReturnSuccess;
}