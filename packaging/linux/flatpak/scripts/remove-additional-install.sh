#!/bin/sh

# User Service
systemctl --user stop app-io.github.Nonary.vibepollo
rm "$HOME/.config/systemd/user/app-io.github.Nonary.vibepollo.service"
systemctl --user daemon-reload
echo "Vibepollo User Service has been removed."

# Remove rules
flatpak-spawn --host pkexec sh -c "rm /etc/modules-load.d/60-sunshine.conf"
flatpak-spawn --host pkexec sh -c "rm /etc/udev/rules.d/60-sunshine.rules"
echo "Input rules removed. Restart computer to take effect."
