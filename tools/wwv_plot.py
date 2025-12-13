#!/usr/bin/env python3
"""
wwv_plot.py - Plot WWV waveform data from wwv_scan --dump output

Usage: python wwv_plot.py <csv_file>

Creates a multi-panel plot showing:
1. Full second overview (envelope and filtered)
2. First 50ms zoomed (where tick should be)
3. Noise region sample (200-250ms)
"""

import sys
import os

try:
    import pandas as pd
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError as e:
    print(f"Missing required package: {e}")
    print("Install with: pip install pandas matplotlib numpy")
    sys.exit(1)

def main():
    if len(sys.argv) < 2:
        print("Usage: python wwv_plot.py <csv_file>")
        print("Example: python wwv_plot.py wwv_waveform.csv")
        sys.exit(1)
    
    csv_file = sys.argv[1]
    
    if not os.path.exists(csv_file):
        print(f"Error: File not found: {csv_file}")
        sys.exit(1)
    
    print(f"Loading {csv_file}...")
    df = pd.read_csv(csv_file)
    
    print(f"Loaded {len(df)} samples")
    print(f"Columns: {list(df.columns)}")
    
    # Sample rate is 48000 Hz
    sample_rate = 48000
    
    # Convert sample number to milliseconds
    df['ms'] = df['sample'] / sample_rate * 1000
    
    # Calculate statistics
    tick_region = df[(df['ms'] >= 0) & (df['ms'] < 10)]  # 0-10ms
    noise_region = df[(df['ms'] >= 200) & (df['ms'] < 800)]  # 200-800ms
    
    tick_envelope_mean = tick_region['envelope'].mean()
    tick_envelope_max = tick_region['envelope'].max()
    tick_filt_mean = tick_region['filtered_1000Hz'].abs().mean()
    tick_filt_max = tick_region['filtered_1000Hz'].abs().max()
    
    noise_envelope_mean = noise_region['envelope'].mean()
    noise_envelope_max = noise_region['envelope'].max()
    noise_filt_mean = noise_region['filtered_1000Hz'].abs().mean()
    noise_filt_max = noise_region['filtered_1000Hz'].abs().max()
    
    # Calculate SNR
    if noise_filt_mean > 0:
        snr_avg = 10 * np.log10(tick_filt_mean / noise_filt_mean)
    else:
        snr_avg = 0
    
    if noise_filt_max > 0:
        snr_peak = 10 * np.log10(tick_filt_max / noise_filt_max)
    else:
        snr_peak = 0
    
    print("\n" + "="*60)
    print("WAVEFORM ANALYSIS")
    print("="*60)
    print(f"\nTick Region (0-10ms):")
    print(f"  Envelope:  mean={tick_envelope_mean:.6f}  max={tick_envelope_max:.6f}")
    print(f"  1000Hz BP: mean={tick_filt_mean:.6e}  max={tick_filt_max:.6e}")
    print(f"\nNoise Region (200-800ms):")
    print(f"  Envelope:  mean={noise_envelope_mean:.6f}  max={noise_envelope_max:.6f}")
    print(f"  1000Hz BP: mean={noise_filt_mean:.6e}  max={noise_filt_max:.6e}")
    print(f"\nSNR (1000Hz filtered):")
    print(f"  Average: {snr_avg:+.1f} dB")
    print(f"  Peak:    {snr_peak:+.1f} dB")
    print("="*60)
    
    # Create figure with subplots
    fig, axes = plt.subplots(4, 1, figsize=(14, 12))
    fig.suptitle(f'WWV Waveform Analysis: {os.path.basename(csv_file)}', fontsize=14)
    
    # Plot 1: Full second - Envelope
    ax1 = axes[0]
    ax1.plot(df['ms'], df['envelope'], 'b-', linewidth=0.5, alpha=0.7)
    ax1.axvspan(0, 10, color='green', alpha=0.2, label='Tick window (0-10ms)')
    ax1.axvspan(200, 800, color='red', alpha=0.1, label='Noise window (200-800ms)')
    ax1.axvline(5, color='green', linestyle='--', linewidth=1, label='5ms (tick end)')
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Envelope')
    ax1.set_title('Full Second - Envelope (sqrt(I² + Q²))')
    ax1.legend(loc='upper right')
    ax1.grid(True, alpha=0.3)
    ax1.set_xlim(0, 1000)
    
    # Plot 2: Full second - 1000Hz Filtered
    ax2 = axes[1]
    ax2.plot(df['ms'], df['filtered_1000Hz'], 'r-', linewidth=0.5, alpha=0.7)
    ax2.axvspan(0, 10, color='green', alpha=0.2)
    ax2.axvspan(200, 800, color='red', alpha=0.1)
    ax2.axvline(5, color='green', linestyle='--', linewidth=1)
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Amplitude')
    ax2.set_title('Full Second - 1000Hz Bandpass Output')
    ax2.grid(True, alpha=0.3)
    ax2.set_xlim(0, 1000)
    
    # Plot 3: Zoomed tick region (0-50ms)
    ax3 = axes[2]
    zoom_df = df[df['ms'] < 50]
    ax3.plot(zoom_df['ms'], zoom_df['envelope'], 'b-', linewidth=1, label='Envelope', alpha=0.7)
    ax3.plot(zoom_df['ms'], zoom_df['filtered_1000Hz'], 'r-', linewidth=1, label='1000Hz filtered')
    ax3.axvspan(0, 5, color='green', alpha=0.3, label='Expected tick (0-5ms)')
    ax3.axvline(5, color='green', linestyle='--', linewidth=2)
    ax3.axvline(10, color='orange', linestyle='--', linewidth=1, label='Tick window end')
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Amplitude')
    ax3.set_title('ZOOMED: First 50ms (Tick Should Be Here)')
    ax3.legend(loc='upper right')
    ax3.grid(True, alpha=0.3)
    
    # Plot 4: I/Q scatter during tick vs noise
    ax4 = axes[3]
    tick_iq = df[(df['ms'] >= 0) & (df['ms'] < 10)]
    noise_iq = df[(df['ms'] >= 200) & (df['ms'] < 210)]  # Just 10ms of noise for comparison
    ax4.scatter(tick_iq['I'], tick_iq['Q'], s=1, alpha=0.5, c='green', label=f'Tick (0-10ms): {len(tick_iq)} pts')
    ax4.scatter(noise_iq['I'], noise_iq['Q'], s=1, alpha=0.5, c='red', label=f'Noise (200-210ms): {len(noise_iq)} pts')
    ax4.set_xlabel('I')
    ax4.set_ylabel('Q')
    ax4.set_title('I/Q Constellation: Tick vs Noise Region')
    ax4.legend()
    ax4.grid(True, alpha=0.3)
    ax4.axis('equal')
    
    plt.tight_layout()
    
    # Save plot
    plot_file = csv_file.replace('.csv', '_plot.png')
    plt.savefig(plot_file, dpi=150)
    print(f"\nPlot saved to: {plot_file}")
    
    # Also show interactive plot
    plt.show()
    
    print("\nKey things to look for:")
    print("  - In Plot 3 (zoomed): Is there a SPIKE in red line during 0-5ms?")
    print("  - If yes: Tick is present, detection algorithm needs tuning")
    print("  - If no:  Tick is not reaching the filter (upstream problem)")
    print("  - In Plot 4: Do tick/noise I/Q patterns look different?")

if __name__ == '__main__':
    main()
