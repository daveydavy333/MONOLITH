# a simple setup script for Monolith, run this inside the cloned directory. this assumes you have the tools needed
meson setup build
ninja -C build
sudo ninja -C build install || doas ninja -C build install
