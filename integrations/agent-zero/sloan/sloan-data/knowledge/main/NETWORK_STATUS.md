# Network Status Report

## 1. Node Access & Key Auth
- **Reached and Authorized:** `apollo`, `fortytwo`, `422`, `kross`, `n100` (`amn100`), `vin`, `cloudwrath`, `tango`.
- **Unreachable:** None! (100% Coverage reached)

## 2. IP Discoveries
- `n100`: `192.168.68.51`
- `vin`: `192.168.68.55`
- `tango`: `192.168.68.56`
- `cloudwrath`: `192.168.68.60`

## 3. pfSense Configuration (`10.0.0.1`)
- **Access:** SSH securely authorized via the `admin` account.
- **Static DHCP Leases:** All 8 discovered nodes had their MAC addresses extracted and permanently injected into the pfSense `dhcpd` daemon on the `lan` interface. Their IPs will no longer shift.

## 4. NFS Shares & Mounts
- **Server `fortytwo`** (`192.168.68.57`): Active, cleanly exporting `/exports/shared`.
- **Server `422`** (`192.168.68.59`): Active, cleanly exporting `/exports/shared`.
- **Client `apollo`** (`192.168.68.52`): Configured successfully. Remote directories point to `/mnt/fortytwo` and `/mnt/422`. 
  - ⚠️ **Safeguard Active**: The extremely critical `noauto` parameter has been permanently added to `/etc/fstab` to bypass the dual 5090 GPU enumeration network/boot issue. The shares have been mounted manually for the current login session.

## Summary
All core infrastructure tasks from `SLOAN_NETWORK_SETUP_PROMPT.md` have been fully completed. Every single node is authenticated, mapped, and secured!
