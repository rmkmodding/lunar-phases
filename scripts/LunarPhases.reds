// Native class implemented in LunarPhases.dll (registered in LunarPhases::RegisterTypes / PostRegisterTypes).
// This file must be deployed at:  red4ext/plugins/LunarPhases/scripts/LunarPhases.reds

public native class LunarPhases extends IScriptable {
    // Set the active moon phase (0 = new moon … 15 = waning crescent).
    public native func SetPhase(phase: Int32) -> Void
    
    // Return the currently active moon phase index (0-15).
    public native func GetPhase() -> Int32
    
    // Set the color of the moon.
    public native func SetMoonColor(r: Float, g: Float, b: Float) -> Void
    
    // Enable or disable star dimming (stars fade as moon brightens, brighten as moon dims).
    public native func SetStarDimming(enabled: Bool) -> Void
    
    // Enable or disable directional-light (moon light + shadows) scaling with the lunar phase.
    // Default true. When false the moon acts as a full-strength light source regardless of phase.
    public native func SetMoonLightScaling(enabled: Bool) -> Void
    
    // Apply a manual brightness multiplier to all star/galaxy rendering (stacks with SetStarDimming).
    // Default 1.0. 0.0 = invisible, 2.0 = double brightness.
    public native func SetStarBrightness(multiplier: Float) -> Void
}
