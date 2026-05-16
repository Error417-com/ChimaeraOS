# xHCI Upgrade Path Notes

This document outlines the architectural changes and implementation steps required to upgrade the ChimeraOS USB subsystem from the current UHCI (Universal Host Controller Interface) implementation to xHCI (eXtensible Host Controller Interface).

## 1. Architectural Differences: UHCI vs. xHCI

The current UHCI driver is designed for USB 1.1 and uses a simple memory-based schedule (Frame List and Transfer Descriptors). Upgrading to xHCI (USB 3.0+) requires a significant paradigm shift:

| Feature | UHCI (Current) | xHCI (Target) |
| :--- | :--- | :--- |
| **Hardware Scope** | USB 1.1 (Low/Full Speed) | USB 1.1, 2.0, 3.x (All Speeds) |
| **Data Structures** | Frame List, TD, QH | Device Context Base Address Array (DCBAA), Command Ring, Event Ring, Transfer Rings |
| **Device Enumeration** | Host software assigns addresses | Host controller assigns addresses via `Address Device` command |
| **Endpoint State** | Managed purely in software/TDs | Managed by hardware via Endpoint Contexts |
| **Interrupts** | Polled or simple IOC interrupts | Event Ring with Event Ring Segment Table (ERST) |

## 2. Required Subsystem Changes

To support xHCI while maintaining backward compatibility (or replacing UHCI entirely), the following core changes are needed:

### 2.1. Host Controller Abstraction Layer (HCD)
Currently, `usb_core.c` directly calls UHCI functions. A proper HCD abstraction layer must be introduced:
- Create a `usb_hcd_t` struct with function pointers for `init`, `poll`, `control_transfer`, and `interrupt_transfer`.
- Register either the UHCI or xHCI driver during PCI enumeration based on the Class/Subclass/ProgIF (xHCI is `0x0C/0x03/0x30`).

### 2.2. Memory Management
xHCI relies heavily on 64-bit physical addresses and strictly aligned memory structures (e.g., 64-byte alignment for Contexts).
- The kernel's `kmalloc` must be extended to support aligned allocations (`kmalloc_aligned`).
- The driver must handle 64-bit physical addresses, even on a 32-bit OS, by writing to the low and high 32-bit registers separately.

## 3. Implementation Steps for xHCI

### Phase 1: Initialization and Ring Setup
1. **PCI Discovery**: Locate the xHCI controller via PCI configuration space.
2. **Capability & Operational Registers**: Map the MMIO space (xHCI uses MMIO, unlike UHCI which uses I/O ports). Read the Capability Registers to find the Operational and Runtime Registers.
3. **DCBAA Setup**: Allocate the Device Context Base Address Array and write its physical address to the `DCBAAP` register.
4. **Command Ring**: Allocate the Command Ring and write to the `CRCR` register.
5. **Event Ring**: Allocate the Event Ring Segment Table (ERST) and the Event Ring itself. Configure the Primary Interrupter.
6. **Start Controller**: Set the Run/Stop (R/S) bit in the `USBCMD` register.

### Phase 2: Device Enumeration
Unlike UHCI where the OS sends a `SET_ADDRESS` request, xHCI handles this in hardware:
1. Detect port connection via the Port Status and Control (`PORTSC`) registers.
2. Issue an `Enable Slot` command on the Command Ring.
3. Wait for the Command Completion Event on the Event Ring to get the assigned Slot ID.
4. Allocate an Input Context and configure the Slot Context and Endpoint 0 Context.
5. Issue an `Address Device` command. The controller will automatically assign a USB address and transition the slot to the Addressed state.

### Phase 3: HID Keyboard/Mouse Support
To support the existing HID boot-protocol drivers over xHCI:
1. **Endpoint Configuration**: Issue a `Configure Endpoint` command to enable the Interrupt IN endpoint for the keyboard/mouse.
2. **Transfer Rings**: Allocate a Transfer Ring for the Interrupt IN endpoint.
3. **Polling**: Enqueue Normal TRBs (Transfer Request Blocks) on the Transfer Ring.
4. **Event Handling**: Instead of polling a TD's active bit (as in UHCI), poll the Event Ring for Transfer Events. When a Transfer Event indicates completion, decode the HID report and enqueue a new TRB.

## 4. Transition Strategy

1. **Keep UHCI Active**: Initially, keep the UHCI driver intact. Many modern systems (or QEMU) provide companion controllers or allow routing ports to UHCI.
2. **Implement xHCI Skeleton**: Add the MMIO mapping and basic ring allocation without disrupting the existing USB core.
3. **Port HID Drivers**: Modify `hid_kbd.c` and `hid_mouse.c` to use the new HCD abstraction layer rather than hardcoded UHCI TD structures.
4. **Testing**: Use QEMU with `-device qemu-xhci` to validate the xHCI implementation in isolation.

## References
[1] eXtensible Host Controller Interface for Universal Serial Bus (xHCI) Requirements Specification, Revision 1.2. Intel Corporation.
