FROM ubuntu:24.04

RUN apt-get update && apt-get install -y build-essential libstdc++6

COPY m68k-unknown-elf /opt/m68k-unknown-elf
ENV PATH="/opt/m68k-unknown-elf/bin:$PATH"
