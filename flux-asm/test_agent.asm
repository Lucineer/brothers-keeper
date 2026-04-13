; test_agent.asm — Agent communication test
; Exercises SPAWN, SEND, RECV, WAIT opcodes

.org 0x0000

    ; Spawn agent with 0 payload bytes
    SPAWN 0

    ; Send with 0 payload (stub in VM)
    SEND 0

    ; Receive with 0 payload (stub in VM)
    RECV 0

    ; Wait for agent with 0 payload
    WAIT 0

    HALT
