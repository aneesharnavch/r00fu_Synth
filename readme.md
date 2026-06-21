
# R00fu Synth
<img width="645" height="452" alt="Cover_pic" src="https://github.com/user-attachments/assets/4408aaff-d5f2-4a53-8d4e-2affb69537de" />

A digital groove box + synth + MIDI controller crammed into one box. 64 buttons , 25 knobs and 15 sliders to compose any beat you can imagine

# How does it work? 
The primary control is done via an ESP32S3 DevcitC-1 , With its two USB-C ports and it runs on two cores. Core 0 does the digital state checking. It scans all 64 buttons , 25 knobs and 15 sliders once every 1ms reading it through a 74HC4067 Analog MUX chip, It also checks for MIDI in Signal and runs the sequencer. Core 1 only uses the I2S protocol driving audio through the PCM5102A.

It connects to your device by either the USB-C port or DIN MIDI sockets. With everything configured using a modded version of an opensource MIDI configuring python script that runs on a lil python GUI. 

# Why did I make it? 

My friend ( on SoundCloud @r00fu ) is really into music , so being the great friend I am was shocked at how expensive most of the music gear he has is . So me being the dumbass I am proceed to underestimated how complex making a Synthesizer is and try to make one. This is my attempt at going for it. 

# Components Required for the Build

| No. | Quantity | Description                                  | Supplier   | Supplier Part        | Product Link                                          | Cost    | Price (USD) |
|-----|----------|----------------------------------------------|------------|----------------------|-------------------------------------------------------|---------|-------------|
| 1   | 7        | 100nF 0402 Decoupling Capacitor              | LCSC       | C2888326             | https://www.lcsc.com/product-detail/C2888326.html     | 0.00093 | 0.00651     |
| 2   | 6        | 10uF 0402 Decoupling Capacitor               | LCSC       | C307415              | https://www.lcsc.com/product-detail/C307415.html      | 0.1492  | 0.8952      |
| 3   | 4        | 1uF 0603 Charge bank Capacitor               | LCSC       | C519560              | https://www.lcsc.com/product-detail/C519560.html      | 0.0181  | 0.0724      |
| 4   | 1        | 3.5mm Headphone Jack                         | LCSC       | C41347691            | https://www.lcsc.com/product-detail/C41347691.html    | 2.273   | 2.273       |
| 5   | 64       | 1N4148W Diode for Switches                   | LCSC       | C917030              | https://www.lcsc.com/product-detail/C917030.html      | 0.0061  | 0.3904      |
| 6   | 3        | 220Ω 0603 Resistor                           | LCSC       | C114683              | https://www.lcsc.com/product-detail/C114683.html      | 0.00017 | 0.00051     |
| 7   | 1        | 10kΩ 0603 Resistor                           | LCSC       | C95204               | https://www.lcsc.com/product-detail/C95204.html       | 0.0241  | 0.0241      |
| 8   | 30       | 10kΩ Knob Potentiometer                      | LCSC       | C209779              | https://www.lcsc.com/product-detail/C209779.html      | 0.8401  | 25.203      |
| 9   | 15       | 10kΩ Slide Potentiometer                     | LCSC       | C5357832             | https://www.lcsc.com/product-detail/C5357832.html     | 2.82    | 42.3        |
| 10  | 2        | DIN connector for MiDi                       | LCSC       | C2939347             | https://www.lcsc.com/product-detail/C2939347.html     | 0.3249  | 0.6498      |
| 11  | 1        | Optocoupler for MiDi In setup                | LCSC       | C20082               | https://www.lcsc.com/product-detail/C20082.html       | 0.5521  | 0.5521      |
| 12  | 1        | Amplifier for Headset/Speaker use            | LCSC       | C1520792             | https://www.lcsc.com/product-detail/C1520792.html     | 2.1817  | 2.1817      |
| 13  | 3        | Analog Multiplexer for device management     | LCSC       | C1545936             | https://www.lcsc.com/product-detail/C1545936.html     | 2.71    | 8.13        |
| 14  | 64       | Mechanical Switches                          | LCSC       | C49234238            | https://www.lcsc.com/product-detail/C49234238.html    | 0.1068  | 6.8352      |
| 15  | 1        | ESP32-S3-DevKitC-1 Wifi & Blutooth Dev Board | Aliexpress | S3-DevkitC-1-N32R16V | https://www.aliexpress.com/item/1005003979778978.html | 15.52   | 15.52       |
| 16  | 1        | PCB                                          | JLCPCB     | NA                   | https://cart.jlcpcb.com/quote?                        | 28.4    | 28.4        |
|     |          |                                              |            |                      |                                                       | Total   | 133.43392   |
# PCB and Schematic 
I designed it in EasyEDA Pro. You can find the Gerber files in the repo. The schematic and PCB are as follows : - 
<img width="4698" height="3326" alt="SCH_Schematic_r00fu_Synth" src="https://github.com/user-attachments/assets/410b8ae2-5451-4ae2-a562-163bb8617471" />
<img width="1112" height="752" alt="PCB_pic " src="https://github.com/user-attachments/assets/a9a8a405-dc3d-4627-8144-07e9cee68e5d" />

# Assembly 

Please do note that these assembly instructions are not final and can change. I've yet to build it, and when I do, I'll update this section to be more accurate. In the meanwhile, don't be afraid to change things up or make use of the several exposes GPIO pins/testing pad to make any modifications you would like to make. 

Note : Soldering Instructions are SUGGESTIONS and are typically personal preference , If your used to doing things a certain way , do not change it. I personally tend to do SMD components close to THT components first , followed by the THT components . 

1. Since most of the components on this board are THT anyways , I would recommend you start things off by getting all 24 SMD components soldered on. 
2. You can do that by just soldering one end of the SMD capacitor or resistor down with a decent bit of flux to get the part in place 
3. you can follow that up by soldering the other end cleaning the solder joint before checking for continuity on the passives with a multimeter to just verify its alright 
4. Now that you should be done with the SMD parts, you can begin with placing all of the THT components into their place which should be pretty obvious based on physical appearance 
5. I like to be lazy so I usually tape it down from the top to hold onto the parts when i flip the board over to solder the THT elements 
6. Once you've placed all of the parts down (The tape part is optional, feel free to continue without it) , flip the board around and flux the joints up and solder them nice and snug .
7. Make sure the  buttons are snug and level with the bottom before you let the solder reflow and solidify on the joints. The case will be slightly misaligned if you don't solder things in all the way down .
8. Inspect everything with your eyes , looking at every component individually. Its recommended to use an Multimeter or an LCR meter to just sanity check all of the values and check for continuity on all of your passives (you can also sanity check the sliders and knobs if you just plug the board with power)
9. Use some Isopropanol to clean the Flux and solder residue off the board and connect it to a power source to make sure it doesn't explode.

# Case 
The case is a beautiful rectangular enclosure with cutouts for the Knobs, Sliders and Switches with room left for the Keycaps to be put on after assembly. I added a hackclub logo to the back for some represent, pictures showing it are here below : - 
<img width="748" height="505" alt="r00fuSynth_TopDown" src="https://github.com/user-attachments/assets/226fd73c-49cd-454b-a682-34298a30ebb9" />
<img width="848" height="552" alt="r00fuSynth_base" src="https://github.com/user-attachments/assets/73a5b95c-f908-4d71-a8cf-23b1ab59f9a0" />

# Setting up the case

The PCB without keycaps on should just drop onto the bottom plate of the case and you can align the top plates without much difficulty. Once your done with that , just slot in 4 M4 screws through the screw holes and shown in this picture below and you should be done : - 
<img width="1126" height="526" alt="r00fuSynth_Assembly_pic" src="https://github.com/user-attachments/assets/c6e9f0c3-bff4-4b4a-a871-142775271c3a" />

# Hardware at a Glance 

| Subsystem | What it is                                           |
| --------- | ---------------------------------------------------- |
| MCU       | ESP32S3-S3-DevkitC1 Dev Board                        |
| Audio     | PCM5102A I2S DAC, 44.1kHz/16-bit stereo audio system |
| Buttons   | 8x8 matrix with key diodes , scanned every 1ms       |
| Knobs     | 25 pots read via 2 MUXes                             |
| Sliders   | 15 Faders read via 3rd MUX                           |
| MUX       | 3 x 74HC4067 (16CH MUX)                              |
| MIDI      | 5 pin DIN in/out boards                              |
| FX        | delay , reverb , chorus , bitcrusher                 |
# Setting up the Firmware 

The firmware builds on PlatformIO (not the Arduino IDE or the ESP32 IDF) as the code is dependent very much so on several other opensource libraries. 

1. Install PlatformIO if you haven't done so yet , run the following commands in terminal
```cmd
pip install platformio 
pio --version 
```
2. PLUG INTO THE CORRECT USB-C PORT. The DevKitC-1  has two USB-C ports. USE THE USB PORT , NOT THE UART ONE. 
3.  Flashing the firmware for the first time needs you to enter board in boot mode so just 
	**hold `BOOT` → tap `RESET` → release `BOOT`**
4. Flash some sort of test sketch to sanity check the board before flashing the firmware using the PlatformIO cli, Just run the following snippet of CMD commands 
```cmd
cd firmware/r00fu_synth
pio run -t uploadfs # settings and partition 
pio run -t upload # the actual firmware
```
once you get the following message on the terminal , You should be done ! 

```cmd
Verifying written data...
Hash of data verified.

Hard resetting via RTS pin...
```
5. Watch it boot:
```cmd
pio device moonitor -b 115200
```
Its done properly if you see : 
```cmd
HELLO r00fu_synth 0.20
LOG r00fu_synth boot complete 
```
pressing any key or move and knob or slider and the serial monitor should spit out the exact id and state of whatever key/knob/slider you've messed with. So you should be done with uploading code onto the board , now time to configure it for your application. 
# Pin map 

This is the wiring the firmware expects. It's all in `include/config.h` , Its the only source of global definitions in the code.

| What                  | name in code | GPIO pin              | Notes                         |
|-----------------------|--------------|-----------------------|-------------------------------|
| Switch Matrix Rows    | ROW_PINS     | 4,5,6,7,15,16,17,18   | Low when not in Use           |
| Switch Matrix Columns | COL_PINS     | 8,9,10,11,12,13,14,21 | Reading with internal pull up |
| MUX select pins       | MUX_SEL_PINS | 35,36,37,38           | Shared by all MUX             |
| MUX A out             | MUX_A_OUT    | 1                     | MUX A read pin                |
| MUX B out             | MUX_B_OUT    | 2                     | MUX B read pin                |
| MUX C out             | MUX_C_OUT    | 3                     | MUX C read pin                |
| I2S Bit Clock         | I2S_BCK_PIN  | 40                    | PCM5102A BCK                  |
| I2S Word Clock        | I2S_LRCK_PIN | 41                    | PCM5102A LRCK                 |
| I2S Data              | I2S_DOUT_PIN | 42                    | PCM5102A DIN                  |
| MIDI OUT              | MIDI_TX_PIN  | 43                    | DIN out with current limit    |
| MIDI IN               | MIDI_RX_PIN  | 44                    | DIN in through optocoupler    |
# Setting up the the Hardware config ( the software that maps the buttons)

This is the part of the firmware that actually makes the Synth work. run the ``gui/r00fu_config.py`` file to communicate with the synth across USB serial. Pressing a physical button should light up a square on the python GUI and you can configure it  a bunch of different applications. To run it , you only really need python and pyserial installed. If you don't have that done already, just run the following command in your terminal. 

```cmd
pip install pyserial
```

1. run the file now and you should see a dropdown to select the boards COM port 
2. Select the board's COM port and hit connect, the status bar should say some form of connected 
3. once you click a button IRL , you should see it flash in the python script, now on the python script , you can pick one of the functions from the panel : 

| Function  | What it does                                                   |
| --------- | -------------------------------------------------------------- |
| Note      | plays a MIDI note (on the synth and out of USB/DIN)            |
| CC        | command to change MIDI controller (127 on press, 0 on release) |
| Program   | sends a program change                                         |
| Transport | Play/ Stop / Record / Continue etc..                           |
| Mode      | switches the whole box to Drum/StepSeq/Keyboard etc..          |
| None      | does nothing                                                   |
4. Push to the Device , Sends the whole command map over serial. This data is Saved 
5.  Save/Load dumps the map to a config .json file 
# First boot and Setup 
- Plug into the USB port and plug in Headphones or an Amp into the PCM5102A's Headphone jack
- It should boot straight into Drum mode (LED should go yellow). assuming you've set the config with drum notes on the hardware config. 
- Turn a knob , the synth should reach. If knobs feel jittery/laggy then please increase the `ANALOG_SMOOTHING` parameter in the code
- Have a blast , it appears to be working as intended 

If you do run into issues ? Most of the time its :- 
- Wrong USB port
- button not mapped to note yet 
- SCK not tied to ground properly
# Some examples of actually using it 

This is the fun part. The box is **5 instruments**, and you flip between them with a button you mapped to **Mode** (or send it from the GUI). The LED colour tells you where you are.

### Mode 0 Drum Machine :
8 drum tracks, the grid is your pads. Each pad is mapped (in the GUI) to a note that triggers a drum voice. Tap pads to play in real time.

### Mode 1 Step Sequencer :

The 64 buttons are physically wired to resemble a keyboard with 64 Switches, Some of the things you can use the boat for are :

- mute/unmute tracks
- parameter locks
- swing 
- pattern chaining

### Mode 2 Keyboard : 

The grid becomes notes. Set you need to setup a  scale In the python script so every key you hit is in-key. Use **chord mode** to slam triads with the buttons and move notes around with the slider

### Mode 3  DAW Controller :

It's a USB-MIDI control surface. So you can just plug the USB port into your computer, open your DAW, and configure the following items :- 
- buttons (Can be mapped to litterally anything a macro can be mapped to )
- 25 knobs (map them to plugin params, sends, whatever)
- 15 sliders  (for faders or volume for individual subtracks)  
    Everything leaves over USB-MIDI for the host to learn. This is the "I want the full DAW but I hate the mouse" mode.

# Code Tweaks / Example implementation

[FIRMWARE] `include/config.h`:
```cpp
static const int ROW_PINS[MATRIX_ROWS] = { 8, 9, 10, 11, 12, 13, 14, 21 }; // COLS
static const int COL_PINS[MATRIX_COLS] = { 4, 5, 6, 7, 15, 16, 17, 18 };   //ROWS
```

`Knobs too jittery / too sluggish:` change the smoothing. Higher = smoother but laggier.
[FIRMWARE] `include/config.h`:
```cpp
#define ANALOG_SMOOTHING 0.10f   
```

`Buttons feel mushy / double-trigger:` tune the debounce (how many stable 1 ms samples before it commits).
[FIRMWARE] `include/config.h`:
```cpp
#define MATRIX_DEBOUNCE_COUNT 6  // lower = snappier but bouncier and vice versa
```

`Want more/less polyphony:` more voices = richer chords but more CPU on the audio core.
[FIRMWARE] `include/config.h`:
```cpp
#define NUM_VOICES 8  
```

# Files 
```cmd
C:.
│   BOM_r00fu_synth.csv
│   readme.md
│   Zine_r00fu_Synth.pdf
│
├───Assets
│       PCB_3D_Render_No_assembly_pic.png
│       PCB_pic .png
│       r00fuSynth_Assembly.png
│       r00fuSynth_Assembly_pic.png
│       r00fuSynth_base.png
│       r00fuSynth_TopDown.png
│       SCH_Schematic_r00fu_Synth.png
│       Zine_r00fu_Synth.png
│
├───CAD_Files
│       r00fusynth.f3z
│       r00fusynth.step
│
├───Firmware
│   ├───gui
│   │   │   r00fu_config.py
│   │   │   README.md
│   │   │
│   │   └───__pycache__
│   │           r00fu_config.cpython-314.pyc
│   │
│   ├───prototype_single_file
│   │       r00fu_synth_prototype.ino
│   │
│   └───r00fu_synth
│       │   platformio.ini
│       │   SETUP.md
│       │
│       ├───data
│       ├───include
│       │       config.h
│       │       events.h
│       │       midi_map.h
│       │
│       └───src
│           │   main.cpp
│           │
│           ├───audio
│           │       adsr.cpp
│           │       adsr.h
│           │       effects.cpp
│           │       effects.h
│           │       filter.cpp
│           │       filter.h
│           │       oscillator.cpp
│           │       oscillator.h
│           │       synth.cpp
│           │       synth.h
│           │       voice.cpp
│           │       voice.h
│           │
│           ├───drivers
│           │       button_matrix.cpp
│           │       button_matrix.h
│           │       config_protocol.cpp
│           │       config_protocol.h
│           │       midi_din.cpp
│           │       midi_din.h
│           │       mux.cpp
│           │       mux.h
│           │       pcm5102.cpp
│           │       pcm5102.h
│           │       usb_midi.cpp
│           │       usb_midi.h
│           │
│           ├───sequencer
│           │       sequencer.cpp
│           │       sequencer.h
│           │
│           ├───storage
│           │       midi_map.cpp
│           │       presets.cpp
│           │       presets.h
│           │       settings.cpp
│           │       settings.h
│           │
│           ├───system
│           │       events.cpp
│           │       tasks.cpp
│           │       tasks.h
│           │
│           └───ui
│                   modes.cpp
│                   modes.h
│
└───PCB_Files
        Gerber_r00fu_Synth.zip
        PickAndPlace_r00fu_Synth .xlsx
        Source_r00fu_Synth .epro
```
# Zine 
<img width="2819" height="4000" alt="Zine_r00fu_Synth" src="https://github.com/user-attachments/assets/96cd2940-c680-4c29-9f4e-8fa782f55ff0" />
