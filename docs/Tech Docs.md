### **Technical Documentation**

#### **User Guide**

Here’s how to use the lighting control panel:

1. **Adjust Brightness**:
    - Turn the knob to the right to increase brightness. Each step increases it by 25% (e.g., 0% → 25% → 50% → 75% → 100%).
    - Turn the knob to the left to decrease brightness.
2. **Audio Feedback**:
    - As you adjust brightness or toggle colors, the system will play a voice announcement. For example:
        - “Twenty-Five Percent” when brightness is set to 25%.
3. **Exit to Main Menu**:
    - Long press the knob to exit the lighting control mode and return to the main menu.

--- 

#### **System Architecture**

The system is built to manage lighting and audio playback using FreeRTOS. It has two main tasks working together:

1. **Lighting Control**: Manages the brightness.
2. **Audio Playback**: Provides voice feedback whenever the lighting state changes.

**How It’s Structured**:

- A **timer-based task** periodically checks for changes in the lighting settings and updates the light output.
- An **audio playback task** runs in parallel and plays announcements like “Fifty Percent” based on changes in brightness.

**How They Work Together**: When the user adjusts the lighting using the control knob, the system updates the lighting configuration and triggers the audio playback task. These tasks communicate using queues and synchronize through semaphores to ensure that everything happens smoothly without conflicts.

Here’s a simple flow:

1. The user interacts with the control knob (turns it or presses it).
2. The system updates the brightness (PWM) 
3. It sends an audio playback request to a queue.
4. The audio playback task picks up the request, plays the appropriate announcement, and signals that the lighting can now be updated.

---

#### **Concurrency Control**

The system uses semaphores and queues to manage shared resources and ensure tasks run without interfering with each other.
#### **Semaphores**

1. **`light_update_semaphore`**:
    
    - Ensures that lighting updates in `light_2color_layer_timer_cb` do not interfere with audio playback in `play_audio_task`.
    - **Flow**:
        - Acquired in `light_2color_layer_timer_cb` before updating the lighting.
        - Released in `play_audio_task` after completing audio playback.
2. **`playback_semaphore`**:
    
    - Prevents concurrent access to the audio playback resource.
    - **Flow**:
        - Acquired in `play_audio_task` when processing audio playback.
        - Released after playback is complete.

#### **Queue (`playback_queue`)**

- **Purpose**: Passes audio playback requests from the event callback (`light_2color_event_cb`) to `play_audio_task`.
- **Flow**:
    - `light_2color_event_cb` enqueues audio levels using `xQueueSend`.
    - `play_audio_task` dequeues them using `xQueueReceive`.

Together, these mechanisms keep everything in sync. For instance, if you turn the knob quickly, the system ensures that the audio plays correctly without missing any updates or causing lighting delays.

--- 

#### **How It All Comes Together**

Here’s a simple breakdown of the system:

1. **User Input**: The knob triggers adjustments to brightness.
2. **Lighting Update**: The lighting task updates the LEDs based on the new settings.
3. **Audio Feedback**: The audio task plays a voice message describing the change.
4. **Synchronization**: Semaphores ensure that lighting updates and audio playback don’t conflict.

#### **Task Structure**

- **Timer Service Task**:
    
    - **Purpose**: Periodically calls the `light_2color_layer_timer_cb` to update lighting state and respond to changes in PWM (Pulse Width Modulation).
    - **Interaction**: Uses `light_update_semaphore` to synchronize with the audio playback task.
- **`play_audio_task`**:
    
    - **Purpose**: Processes audio playback requests sent via `playback_queue`.
    - **Interaction**: Uses `playback_semaphore` to ensure exclusive access to audio playback resources.

--- 

#### **Interaction Diagram**

```plaintext
User Input (Knob Turn) → light_2color_event_cb
   ↓
Update PWM → xQueueSend to playback_queue
   ↓
play_audio_task (Unblocks) → Audio Playback
   ↓
xSemaphoreGive (light_update_semaphore)
   ↓
light_2color_layer_timer_cb (Updates Lighting)
```
