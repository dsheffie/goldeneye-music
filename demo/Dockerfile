# Toolchain image for the royalty-free demo ROM: libdragon's public-domain SDK
# installed into its own prebuilt mips64-elf toolchain.  Built once, so building the
# ROM afterwards is fast and reproducible for anyone with docker.
FROM ghcr.io/dragonminded/libdragon:latest

ARG LIBDRAGON_REF=trunk
RUN git clone --depth 1 -b ${LIBDRAGON_REF} https://github.com/DragonMinded/libdragon.git /libdragon \
 && cd /libdragon \
 && N64_INST=/n64_toolchain ./build.sh \
 && rm -rf /libdragon/.git
ENV N64_INST=/n64_toolchain
