idf.py qemu --qemu-extra-args "-nic user,model=open_eth,hostfwd=tcp::8080-:80 -serial null -serial pty" ${1}
