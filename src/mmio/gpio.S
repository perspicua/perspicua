.include "src/mmio/mmio.inc"
.include "src/mmio/gpio.inc"

.global gpio_set_pin_function

# x0 - pin number
# x1 - pin function
gpio_set_pin_function:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    ldr x2, =GPIO_GPFSEL0
    mov x3, #3

    bl gpio_write
    
    ldp x29, x30, [sp], #16
    ret

# x0 - pin number
# x1 - value to write
# x2 - base register
# x3 - field width
gpio_write:
    # make sure the pin number <= GPIO_MAX_PIN
    cmp x0, GPIO_MAX_PIN
    b.le gpio_write_okpin
    ret

gpio_write_okpin:
    # Calculate register offset for pin
    mov x5, #32
    udiv x4, x5, x3 // x4 = number of pins per 4 byte register

    udiv x5, x0, x4
    mov x7, #4
    madd x2, x5, x6, x2 // x2 = address to write to

    # Create the mask for the value
    mov w4, #1 
    lsl w4, w4, w3
    sub w4, w4, #1

    ldr w3, [x2] // Read the current register value

    ret
