#!/usr/bin/env python3
"""
Bluetooth NoInputNoOutput agent — auto-accepts all pairing requests.
Required for CarPlay and Android Auto phone connections.
"""
import dbus
import dbus.service
import dbus.mainloop.glib
from gi.repository import GLib
import subprocess
import sys

AGENT_INTERFACE = "org.bluez.Agent1"
AGENT_PATH = "/org/aadongle/agent"

class Agent(dbus.service.Object):
    @dbus.service.method(AGENT_INTERFACE, in_signature="", out_signature="")
    def Release(self):
        pass

    @dbus.service.method(AGENT_INTERFACE, in_signature="os", out_signature="")
    def AuthorizeService(self, device, uuid):
        print(f"bt-agent: authorize service {uuid} for {device}")

    @dbus.service.method(AGENT_INTERFACE, in_signature="o", out_signature="s")
    def RequestPinCode(self, device):
        print(f"bt-agent: pin request from {device}")
        return "0000"

    @dbus.service.method(AGENT_INTERFACE, in_signature="o", out_signature="u")
    def RequestPasskey(self, device):
        print(f"bt-agent: passkey request from {device}")
        return dbus.UInt32(0)

    @dbus.service.method(AGENT_INTERFACE, in_signature="ouq", out_signature="")
    def DisplayPasskey(self, device, passkey, entered):
        pass

    @dbus.service.method(AGENT_INTERFACE, in_signature="ou", out_signature="")
    def RequestConfirmation(self, device, passkey):
        print(f"bt-agent: auto-confirming pairing with {device} passkey={passkey}")
        # Auto-accept — no user interaction needed

    @dbus.service.method(AGENT_INTERFACE, in_signature="o", out_signature="")
    def RequestAuthorization(self, device):
        print(f"bt-agent: auto-authorizing {device}")

    @dbus.service.method(AGENT_INTERFACE, in_signature="", out_signature="")
    def Cancel(self):
        pass

def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    agent = Agent(bus, AGENT_PATH)

    manager = dbus.Interface(
        bus.get_object("org.bluez", "/org/bluez"),
        "org.bluez.AgentManager1"
    )

    manager.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
    manager.RequestDefaultAgent(AGENT_PATH)
    print("bt-agent: registered NoInputNoOutput agent, auto-accepting all pairing")

    # Ensure adapter is discoverable
    adapter = dbus.Interface(
        bus.get_object("org.bluez", "/org/bluez/hci0"),
        "org.freedesktop.DBus.Properties"
    )
    adapter.Set("org.bluez.Adapter1", "Powered", dbus.Boolean(True))
    adapter.Set("org.bluez.Adapter1", "Discoverable", dbus.Boolean(True))
    adapter.Set("org.bluez.Adapter1", "DiscoverableTimeout", dbus.UInt32(0))
    adapter.Set("org.bluez.Adapter1", "Pairable", dbus.Boolean(True))
    adapter.Set("org.bluez.Adapter1", "PairableTimeout", dbus.UInt32(0))
    adapter.Set("org.bluez.Adapter1", "Alias", dbus.String("AADongle"))

    print("bt-agent: adapter configured — discoverable, pairable, no timeout")

    loop = GLib.MainLoop()
    loop.run()

if __name__ == "__main__":
    main()
