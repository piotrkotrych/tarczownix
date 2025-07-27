# TARCZOWNIX Motor Control System

A sophisticated ESP32-based motor control system for managing 6 relays controlling 3 motor pairs with web-based configuration and monitoring.

## Features

### 🚀 Core Functionality
- **3 Motor Pairs**: Controls 6 relays organized in 3 alternating pairs (0↔1, 2↔3, 4↔5)
- **Safety-First Design**: Only one relay per motor pair can be active at any time
- **1-Second Timeout Protection**: Automatic shutdown if motors don't reach limit switches
- **Configurable Delays**: Individual min/max delay settings for each relay (100ms - 20s)
- **Persistent Configuration**: Settings saved to ESP32 flash memory

### 🌐 Web Interface
- **Real-time Monitoring**: Live status updates with auto-refresh
- **Motor Pair Visualization**: Clear status display for each motor pair
- **Individual Relay Configuration**: Separate delay settings for each relay
- **Error Tracking**: Comprehensive error logging and display
- **Responsive Design**: Mobile-friendly interface with modern styling

### 🛡️ Safety Features
- **I2C Communication Monitoring**: Detects and reports communication failures
- **Input Debouncing**: 50ms debounce prevents false triggers
- **Emergency Shutdown**: Immediate stop on safety violations
- **Mutual Exclusion**: Prevents both relays in a pair from being active
- **System State Tracking**: STOPPED/RUNNING/ERROR states

### 📡 API Endpoints
- `GET /` - Main web interface
- `GET /start` - Start motor sequence (relays 0, 2, 4)
- `GET /stop` - Emergency stop all motors
- `GET /status` - JSON status data with real-time information
- `GET /config` - Current configuration in JSON format
- `GET /set-delay?relay=X&min=Y&max=Z` - Update relay timing
- `GET /clear-error` - Clear error messages

## Hardware Requirements

### Components
- **ESP32 NodeMCU-32S** (main controller)
- **2x PCF8574 I2C Expanders**:
  - Address `0x22` - Input expander (limit switches)
  - Address `0x24` - Relay expander (motor control)
- **6x Relays** capable of switching 12V motor loads
- **6x Limit Switches** (normally open, connected to inputs)
- **3x Motors** (12V DC or appropriate voltage)

### Wiring
```
ESP32 NodeMCU-32S:
- GPIO 4  → SDA (both PCF8574s)
- GPIO 15 → SCL (both PCF8574s)
- 3.3V    → VCC (both PCF8574s)
- GND     → GND (both PCF8574s)

PCF8574 (0x22) - Inputs:
- P0-P5 → Limit switches (NO contacts to GND)

PCF8574 (0x24) - Relays:
- P0-P5 → Relay control inputs
```

## Software Configuration

### Build Flags
The system includes optimized build flags for performance:
```ini
build_flags = 
    -D CONFIG_ASYNC_TCP_STACK_SIZE=4096     ; Optimized stack size
    -D CONFIG_ASYNC_TCP_RUNNING_CORE=1      ; Core affinity
    -D CONFIG_ASYNC_TCP_MAX_ACK_TIME=5000   ; Connection timeout
    -D PCF8574_LOW_LATENCY                  ; Fast I2C response
    -D CORE_DEBUG_LEVEL=3                   ; Debug output
```

### Library Dependencies
- `xreef/PCF8574 library@^2.3.7` - I2C expander control
- `esp32async/ESPAsyncWebServer@^3.7.6` - Asynchronous web server

## Operation

### Startup Sequence
1. System initializes in STOPPED state
2. WiFi Access Point created: "ESP32-Access-Point" / "pass"
3. Web interface available at: `http://192.168.1.111`
4. All relays OFF, ready for manual start

### Motor Control Logic
1. **Start**: User activates relays 0, 2, and 4 simultaneously
2. **Detection**: When limit switch triggers, corresponding relay turns OFF
3. **Delay**: Configurable random delay (min-max range)
4. **Alternation**: Partner relay turns ON (0↔1, 2↔3, 4↔5)
5. **Timeout**: If no limit switch in 1 second → Emergency stop

### Safety Systems
- **Mutual Exclusion**: Software prevents both relays in pair from being ON
- **Timeout Protection**: 1-second maximum motor run time
- **I2C Monitoring**: Detects communication failures
- **Error Recovery**: System stops and reports issues via web interface

## Improvements Over Original

### Code Structure
- ✅ Replaced individual variables (r0-r5, z0-z5) with structured arrays
- ✅ Added proper state machine with system states
- ✅ Implemented loop-based processing for cleaner code

### Safety Enhancements
- ✅ Added I2C communication error checking
- ✅ Implemented motor safety checks (mutual exclusion)
- ✅ Enhanced input debouncing (50ms)
- ✅ System state tracking with error recovery

### Web Interface Improvements
- ✅ Modern, responsive design with CSS Grid
- ✅ Real-time status updates with auto-refresh
- ✅ Motor pair visualization
- ✅ Enhanced error display and management
- ✅ Better mobile compatibility

### API Enhancements
- ✅ Extended `/status` endpoint with comprehensive data
- ✅ Added `/config` endpoint for configuration retrieval
- ✅ JSON-formatted responses for integration
- ✅ System metrics (uptime, free heap)

### Performance Optimizations
- ✅ Optimized AsyncTCP configuration
- ✅ Low-latency PCF8574 mode
- ✅ Efficient loop structure
- ✅ Reduced memory usage

## Future Enhancement Suggestions

### Advanced Features
1. **MQTT Integration**: Remote monitoring and control
2. **Data Logging**: Cycle counts, timing analytics
3. **Predictive Maintenance**: Motor performance tracking
4. **OTA Updates**: Over-the-air firmware updates
5. **Multi-language Support**: Internationalization

### Safety Improvements
1. **Redundant Sensors**: Dual limit switches per motor
2. **Current Monitoring**: Motor load analysis
3. **Temperature Sensors**: Thermal protection
4. **Watchdog Timer**: Hardware-level failsafe

### UI/UX Enhancements
1. **WebSocket Integration**: Real-time updates without refresh
2. **Historical Data**: Graphs and analytics
3. **User Authentication**: Access control
4. **Mobile App**: Native mobile application

## Troubleshooting

### Common Issues
1. **Motors don't stop**: Check limit switch wiring and PCF8574 address 0x22
2. **Relays don't activate**: Verify PCF8574 address 0x24 and relay connections
3. **Web interface inaccessible**: Confirm WiFi connection to "ESP32-Access-Point"
4. **I2C errors**: Check SDA/SCL connections and pull-up resistors

### Debug Output
Monitor serial output at 115200 baud for detailed system information:
- I2C communication status
- Input state changes
- Relay activation/deactivation
- Error messages and system state changes

## License
This project is provided as-is for educational and industrial automation purposes.
