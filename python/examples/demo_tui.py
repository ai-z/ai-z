#!/usr/bin/env python3
"""
AI-Z Demo TUI - Demonstrates the Python metrics API with a live-updating display.

This demo mimics the C++ AI-Z TUI, showing real-time CPU, RAM, GPU, disk, and network metrics.

Requirements:
    pip install ai-z[demo]
    # or: pip install ai-z rich

Usage:
    python demo_tui.py
"""

import time
import sys

try:
    import aiz
except ImportError:
    print("Error: ai-z package not installed. Run: pip install ai-z")
    sys.exit(1)

try:
    from rich.console import Console
    from rich.table import Table
    from rich.live import Live
    from rich.panel import Panel
    from rich.layout import Layout
    from rich.text import Text
except ImportError:
    print("Error: rich package not installed. Run: pip install rich")
    sys.exit(1)


def format_optional(value, suffix: str = "", precision: int = 1) -> str:
    """Format an optional value, returning '-' if None."""
    if value is None:
        return "-"
    return f"{value:.{precision}f}{suffix}"


def create_cpu_ram_table(cpu_pct, cpu_max_core, ram) -> Table:
    """Create a table showing CPU and RAM metrics."""
    table = Table(title="CPU & RAM", expand=True)
    table.add_column("Metric", style="cyan")
    table.add_column("Value", style="green", justify="right")
    
    table.add_row("CPU Usage", format_optional(cpu_pct, "%"))
    table.add_row("CPU Max Core", format_optional(cpu_max_core, "%"))
    
    if ram:
        table.add_row("RAM Used", f"{ram.used_gib:.1f} GiB")
        table.add_row("RAM Total", f"{ram.total_gib:.1f} GiB")
        table.add_row("RAM %", f"{ram.used_pct:.1f}%")
    else:
        table.add_row("RAM", "-")
    
    return table


def create_gpu_table(gpus: list) -> Table:
    """Create a table showing GPU metrics."""
    table = Table(title="GPUs", expand=True)
    table.add_column("GPU", style="cyan")
    table.add_column("Util", justify="right")
    table.add_column("VRAM", justify="right")
    table.add_column("Temp", justify="right")
    table.add_column("Power", justify="right")
    table.add_column("Source", style="dim")
    
    if not gpus:
        table.add_row("No GPUs detected", "-", "-", "-", "-", "-")
    else:
        for gpu in gpus:
            name = gpu.name[:30] if len(gpu.name) > 30 else gpu.name
            util = format_optional(gpu.util_pct, "%")
            
            if gpu.vram_used_gib is not None and gpu.vram_total_gib is not None:
                vram = f"{gpu.vram_used_gib:.1f}/{gpu.vram_total_gib:.1f} GiB"
            else:
                vram = "-"
            
            temp = format_optional(gpu.temp_c, "°C", 0)
            power = format_optional(gpu.power_watts, "W", 0)
            
            table.add_row(name, util, vram, temp, power, gpu.source)
    
    return table


def create_io_table(disk_read, disk_write, net_rx, net_tx) -> Table:
    """Create a table showing disk and network I/O."""
    table = Table(title="I/O Bandwidth", expand=True)
    table.add_column("Metric", style="cyan")
    table.add_column("Value", style="green", justify="right")
    
    table.add_row("Disk Read", format_optional(disk_read, " MB/s"))
    table.add_row("Disk Write", format_optional(disk_write, " MB/s"))
    table.add_row("Network RX", format_optional(net_rx, " MB/s"))
    table.add_row("Network TX", format_optional(net_tx, " MB/s"))
    
    return table


def create_hardware_panel(hw: aiz.HardwareInfo) -> Panel:
    """Create a panel showing hardware info."""
    lines = [
        f"[cyan]OS:[/] {hw.os_pretty}",
        f"[cyan]Kernel:[/] {hw.kernel_version}",
        f"[cyan]CPU:[/] {hw.cpu_name}",
        f"[cyan]Cores:[/] {hw.cpu_physical_cores} physical, {hw.cpu_logical_cores} logical",
        f"[cyan]RAM:[/] {hw.ram_summary}",
        f"[cyan]GPU:[/] {hw.gpu_name}",
    ]
    if hw.cuda_version:
        lines.append(f"[cyan]CUDA:[/] {hw.cuda_version}")
    if hw.nvml_version:
        lines.append(f"[cyan]NVML:[/] {hw.nvml_version}")
    if hw.rocm_version:
        lines.append(f"[cyan]ROCm:[/] {hw.rocm_version}")
    
    return Panel("\n".join(lines), title="Hardware Info", border_style="blue")


def main():
    console = Console()
    
    console.print("[bold blue]AI-Z Python Demo TUI[/]")
    console.print("Press Ctrl+C to exit\n")
    
    # Probe hardware info
    console.print("[dim]Probing hardware...[/]")
    hw = aiz.probe_hardware()
    console.print(create_hardware_panel(hw))
    console.print()
    
    # Prime the collectors
    console.print("[dim]Initializing metrics collectors...[/]")
    metrics = aiz.Metrics()
    metrics.prime()
    time.sleep(0.5)
    
    console.print("[dim]Starting live monitoring (500ms refresh)...[/]\n")
    
    def generate_display():
        snapshot = metrics.sample()
        
        layout = Layout()
        layout.split_column(
            Layout(name="top", ratio=1),
            Layout(name="bottom", ratio=1),
        )
        
        layout["top"].split_row(
            Layout(create_cpu_ram_table(
                snapshot["cpu_pct"],
                snapshot["cpu_max_core_pct"],
                snapshot["ram"]
            )),
            Layout(create_io_table(
                snapshot["disk_read_mbps"],
                snapshot["disk_write_mbps"],
                snapshot["network_rx_mbps"],
                snapshot["network_tx_mbps"]
            )),
        )
        
        layout["bottom"].update(create_gpu_table(snapshot["gpus"]))
        
        return layout
    
    try:
        with Live(generate_display(), console=console, refresh_per_second=2) as live:
            while True:
                time.sleep(0.5)
                live.update(generate_display())
    except KeyboardInterrupt:
        console.print("\n[yellow]Exiting...[/]")


if __name__ == "__main__":
    main()
