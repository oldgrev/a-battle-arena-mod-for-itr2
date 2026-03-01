# Troubleshooting

Common issues and solutions for the Battle Arena Mod.

## Installation Issues

### Mod Not Loading

**Symptoms:**
- Can't connect to port 7777
- No mod features available

**Solutions:**

1. **Verify file locations:**
   ```
   Win64/
   ├── version.dll          ← Must be here
   └── Mods/
       └── battlearena.dll  ← Must be in Mods folder
   ```

2. **Check file names:**
   - Must be exactly `version.dll` (not `version (1).dll`)
   - Must be exactly `battlearena.dll`

3. **Create the Mods folder** if it doesn't exist

4. **Run as Administrator** if files copied but mod doesn't load

5. **Check Windows Defender:**
   - May quarantine the DLL files
   - Add an exception for the game folder

### Can't Connect to TCP Server

**Symptoms:**
- `telnet localhost 7777` fails
- "Connection refused" error

**Solutions:**

1. **Ensure the game is running** and you're in a level

2. **Wait a few seconds** after game launch - the mod needs time to initialize

3. **Check for port conflicts:**
   ```powershell
   netstat -ano | findstr 7777
   ```
   If another process is using port 7777, close it

4. **Check firewall:**
   - Allow the game through Windows Firewall
   - Port 7777 only needs localhost access

5. **Check the log file** for errors (see below)

### Finding the Log File

The mod logs to:
```
%USERPROFILE%\AppData\Local\ITR2Mod\mod.log
```

Or in expanded form:
```
C:\Users\<YourUsername>\AppData\Local\ITR2Mod\mod.log
```

Look for lines containing `[mod]` or `ERROR`.

---

## Runtime Issues

### Commands Return "World not ready"

**Cause:** You're in the main menu or a level is loading.

**Solution:** Wait until you're fully loaded into a game level.

### Arena Not Starting

**Symptoms:**
- `arena_start` returns success but no enemies appear

**Solutions:**

1. **Ensure you're in a valid level** (not main menu, shelter)

2. **Check NPC database:**
   - Look for `npcs.txt` next to the game executable
   - If missing or empty, NPCs haven't been discovered yet

3. **Try a smaller wave size:**
   ```
   arena_start 5
   ```

4. **Check the log** for spawning errors

### Enemies Spawning Behind Me

**Cause:** The escape direction system is working, but enemies may still spawn in unexpected locations.

**Understanding the system:**
- At wave start, the mod captures your "escape direction" (where you're looking)
- Enemies are blocked from spawning in that hemisphere
- If you turn around, enemies may already be spawning there

**Solution:** Face the direction you want enemies to come from when starting the arena.

### Enemies Not Moving

**Symptoms:**
- NPCs spawn but stand still
- Arena wave never completes

**Solutions:**

1. **Wait for anti-stuck:**
   - After 10 seconds, immobile NPCs may teleport closer
   - After 120 seconds, fully stuck NPCs are culled

2. **Check if NPCs are blocked:**
   - Terrain obstacles
   - Pathfinding issues

3. **Use arena_stop and restart:**
   ```
   arena_stop
   arena_start
   ```

### Friend NPC Attacking Player

**This shouldn't happen** - if it does:

**Solutions:**

1. **Clear and respawn:**
   ```
   friend_clear
   friend
   ```

2. **Check for mod conflicts** if using other mods

3. **Report the bug** with your log file

### Performance Issues

**Symptoms:**
- Frame drops during arena
- Stuttering when waves start

**Solutions:**

1. **Reduce wave size:**
   ```
   arena_start 10
   ```

2. **Increase spawn distance** (spreads out spawn timing):
   ```
   arena_start 20 20000
   ```

3. **The pre-spawn system** should minimize hitching - if not working:
   - Check the log for spawn errors
   - Some NPC types may load slowly

### Cheats Not Working

**Symptoms:**
- Toggle cheats but no effect

**Specific issues:**

| Cheat | Common Cause | Solution |
|-------|--------------|----------|
| God Mode | New damage source | Check after damage type |
| Unlimited Ammo | Weapon type | Some weapons may not be hooked |
| Bullet Time | Scale conflicts | Try `bullettime 0.2` explicitly |
| Lights | No lights equipped | Must be holding flashlight/headlamp |

### Loadout Not Applying

**Symptoms:**
- `apply` command runs but items don't appear

**Solutions:**

1. **Check loadout exists:**
   ```
   loadouts
   ```

2. **Verify file format:**
   - Open the `.loadout` file in a text editor
   - Should have `[LOADOUT]` header

3. **Check item availability:**
   - Some items may require specific game progress
   - Check the log for spawn failures

---

## Crashes

### Crash on Startup

**Solutions:**

1. **Verify game integrity** through Steam

2. **Remove the mod** and test vanilla game

3. **Check for mod conflicts:**
   - Remove other mods
   - Test with just this mod

4. **Update the mod** to latest version

### Crash During Arena

**Possible causes:**
- Too many enemies
- Corrupted NPC data

**Solutions:**

1. **Reduce wave size**

2. **Delete npcs.txt** to force re-discovery:
   ```
   del npcs.txt
   ```

3. **Check the log** for the crash point

### Crash When Loading Loadout

**Possible causes:**
- Corrupted loadout file
- Game updated and items changed

**Solutions:**

1. **Try a different loadout**

2. **Delete the problematic loadout** and recapture

3. **Manually check** the loadout file for corruption

---

## Known Limitations

### TCP Server Security
- No authentication on port 7777
- Don't use on public networks
- Local access only recommended

### VR Menu
- May not render in all lighting conditions
- Widget-based approach is experimental
- Some menu items may not function during certain game states

### Arena Mode
- Only works with discovered NPC types
- Some enemy behaviors may be unpredictable
- Time lock only affects the current level

### Loadouts
- Item transforms may drift slightly
- Some attachment combinations may fail
- Dynamic items (crafted) may not restore correctly

---

## Getting Help

### Information to Provide

When reporting issues, include:

1. **Your mod version**
2. **Game version/patch level**
3. **The log file** (`%USERPROFILE%\AppData\Local\ITR2Mod\mod.log`)
4. **Steps to reproduce**
5. **Any error messages**

### Log File Analysis

Look for these patterns:

```
[ERROR] - Something went wrong
[WARN] - Potential issue
[mod] Starting - Mod initialized
[mod] TCP server started - Commands should work
[Arena] - Arena-related messages
[Friend] - Friend NPC messages
```

### Common Log Errors

| Error | Meaning |
|-------|---------|
| `Another mod instance is already running` | Mod loaded twice - restart game |
| `Failed to start TCP server` | Port 7777 in use |
| `GObjects not ready` | Game still initializing |
| `World not ready` | Still in loading screen |
