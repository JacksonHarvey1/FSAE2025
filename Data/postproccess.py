"""
FSAE Data Post-Processing Script
Generates Excel workbooks with graphs from CSV sensor data
"""

import pandas as pd
import numpy as np
from openpyxl import Workbook
from openpyxl.chart import LineChart, Reference
from openpyxl.chart.axis import ChartLines
from openpyxl.utils.dataframe import dataframe_to_rows
import os
from pathlib import Path
import tkinter as tk
from tkinter import ttk

def select_columns_dialog(columns, title_text="Select which columns to create graphs for:"):
    """
    Create a dialog for user to select which columns to graph
    
    Args:
        columns: List of column names
        title_text: Title text for the dialog
    
    Returns:
        List of selected column names
    """
    selected_columns = []
    
    def on_submit():
        nonlocal selected_columns
        selected_columns = [col for col, var in checkboxes.items() if var.get()]
        root.quit()
        root.destroy()
    
    def select_all():
        for var in checkboxes.values():
            var.set(True)
    
    def deselect_all():
        for var in checkboxes.values():
            var.set(False)
    
    # Create main window
    root = tk.Tk()
    root.title("Select Columns")
    root.geometry("600x700")
    
    # Title label
    title_label = tk.Label(root, text=title_text, 
                           font=("Arial", 12, "bold"))
    title_label.pack(pady=10)
    
    # Button frame
    button_frame = tk.Frame(root)
    button_frame.pack(pady=5)
    
    select_all_btn = tk.Button(button_frame, text="Select All", command=select_all)
    select_all_btn.pack(side=tk.LEFT, padx=5)
    
    deselect_all_btn = tk.Button(button_frame, text="Deselect All", command=deselect_all)
    deselect_all_btn.pack(side=tk.LEFT, padx=5)
    
    # Create scrollable frame for checkboxes
    canvas = tk.Canvas(root)
    scrollbar = ttk.Scrollbar(root, orient="vertical", command=canvas.yview)
    scrollable_frame = ttk.Frame(canvas)
    
    scrollable_frame.bind(
        "<Configure>",
        lambda e: canvas.configure(scrollregion=canvas.bbox("all"))
    )
    
    canvas.create_window((0, 0), window=scrollable_frame, anchor="nw")
    canvas.configure(yscrollcommand=scrollbar.set)
    
    # Create checkboxes
    checkboxes = {}
    for col in columns:
        if col != 'Time (sec)':  # Don't include time column
            var = tk.BooleanVar(value=True)  # Default to selected
            checkboxes[col] = var
            cb = ttk.Checkbutton(scrollable_frame, text=col, variable=var)
            cb.pack(anchor='w', padx=20, pady=2)
    
    canvas.pack(side="left", fill="both", expand=True, padx=(20, 0))
    scrollbar.pack(side="right", fill="y")
    
    # Submit button
    submit_btn = tk.Button(root, text="Continue", command=on_submit, 
                          bg="green", fg="white", font=("Arial", 10, "bold"))
    submit_btn.pack(pady=20)
    
    root.mainloop()
    
    return selected_columns

def get_combined_workbook_name():
    """
    Simple dialog to get the name for combined workbook
    
    Returns:
        String with workbook name
    """
    workbook_name = ""
    
    def on_submit():
        nonlocal workbook_name
        workbook_name = name_entry.get().strip()
        root.quit()
        root.destroy()
    
    root = tk.Tk()
    root.title("Combined Workbook Name")
    root.geometry("400x150")
    
    label = tk.Label(root, text="Enter name for combined workbook:", 
                     font=("Arial", 11))
    label.pack(pady=20)
    
    name_entry = tk.Entry(root, width=30, font=("Arial", 10))
    name_entry.insert(0, "Combined_Analysis")
    name_entry.pack(pady=10)
    
    submit_btn = tk.Button(root, text="OK", command=on_submit, 
                          bg="green", fg="white", font=("Arial", 10, "bold"))
    submit_btn.pack(pady=10)
    
    root.mainloop()
    
    return workbook_name if workbook_name else "Combined_Analysis"

def add_chart_to_sheet(wb, sheet_name, data_sheet, time_col, data_col, title, df, time_col_name, data_col_name):
    """
    Add a line chart to a new sheet in the workbook with data
    
    Args:
        wb: Workbook object
        sheet_name: Name for the new chart sheet
        data_sheet: Sheet containing the data
        time_col: Column index for time (x-axis)
        data_col: Column index for data (y-axis)
        title: Chart title
        df: DataFrame containing the data
        time_col_name: Name of time column
        data_col_name: Name of data column
    """
    # Create new sheet for chart
    chart_sheet = wb.create_sheet(sheet_name)
    
    # Copy relevant data to this sheet (starting at row 1)
    chart_sheet.cell(1, 1, time_col_name)
    chart_sheet.cell(1, 2, data_col_name)
    
    for idx, (time_val, data_val) in enumerate(zip(df[time_col_name], df[data_col_name]), start=2):
        chart_sheet.cell(idx, 1, time_val)
        chart_sheet.cell(idx, 2, data_val)
    
    # Create simple line chart (no fancy colors)
    chart = LineChart()
    chart.title = title
    chart.style = None  # Remove default styling
    chart.x_axis.title = "Time (sec)"
    chart.y_axis.title = title
    chart.width = 20
    chart.height = 12
    
    # Add gridlines for both axes
    chart.x_axis.majorGridlines = ChartLines()
    chart.y_axis.majorGridlines = ChartLines()
    
    # Remove legend
    chart.legend = None
    
    # Add data series from the sheet itself
    y_values = Reference(chart_sheet, min_col=2, min_row=1, max_row=len(df)+1)
    x_values = Reference(chart_sheet, min_col=1, min_row=2, max_row=len(df)+1)
    
    chart.add_data(y_values, titles_from_data=True)
    chart.set_categories(x_values)
    
    # Simple line style - no colors
    if chart.series:
        chart.series[0].graphicalProperties.line.solidFill = "000000"  # Black line
        chart.series[0].smooth = True  # Smooth line
    
    # Add chart to sheet (positioned to the right of data)
    chart_sheet.add_chart(chart, "D2")

def process_csv_file(csv_path, selected_columns=None):
    """
    Process a single CSV file and generate Excel workbook with graphs
    
    Args:
        csv_path: Path to CSV file
        selected_columns: List of column names to graph (if None, will show dialog)
    """
    print(f"\nProcessing: {csv_path}")
    
    # Read file and find where actual CSV data starts
    with open(csv_path, 'r', encoding='latin-1', errors='ignore') as f:
        lines = f.readlines()
    
    # Find the line that starts with "Time" (the header)
    header_line = 0
    for i, line in enumerate(lines):
        if line.startswith('Time'):
            header_line = i
            break
    
    # Read CSV starting from header line
    df = pd.read_csv(csv_path, encoding='latin-1', skiprows=header_line, on_bad_lines='skip')
    
    # Clean column names (remove extra spaces and invalid characters)
    df.columns = df.columns.str.strip()
    
    # Remove any columns with special characters or metadata
    valid_columns = []
    for col in df.columns:
        # Keep only columns that are proper sensor names (ASCII printable)
        if all(ord(c) < 128 for c in str(col)):
            valid_columns.append(col)
    
    df = df[valid_columns]
    
    # Remove rows with invalid data (metadata that got through)
    # Keep only rows where Time column is numeric
    df = df[pd.to_numeric(df['Time (sec)'], errors='coerce').notna()]
    
    # Reset index after filtering
    df = df.reset_index(drop=True)
    
    # Convert all columns to numeric where possible
    for col in df.columns:
        df[col] = pd.to_numeric(df[col], errors='ignore')
    
    # Show column selection dialog if not provided
    if selected_columns is None:
        print("  Opening column selection dialog...")
        selected_columns = select_columns_dialog(list(df.columns))
        
        if not selected_columns:
            print("  No columns selected. Skipping this file.\n")
            return
    
    print(f"  Creating graphs for {len(selected_columns)} selected columns...")
    
    # Create output filename
    output_path = csv_path.replace('.csv', '_analysis.xlsx')
    
    # Create workbook
    wb = Workbook()
    wb.remove(wb.active)  # Remove default sheet
    
    # Add raw data sheet
    data_sheet = wb.create_sheet("Raw Data")
    for r in dataframe_to_rows(df, index=False, header=True):
        data_sheet.append(r)
    
    # Get time column index (should be column 1)
    time_col_idx = 1
    
    # Create graphs for selected columns
    chart_count = 0
    for col in selected_columns:
        if col in df.columns and col != 'Time (sec)':
            try:
                col_idx = df.columns.get_loc(col) + 1
                safe_name = col.replace('/', '_').replace('(', '').replace(')', '').replace('#', '').replace(' ', '_')[:31]
                sheet_name = safe_name[:31]
                
                # Ensure unique sheet names
                base_name = sheet_name
                counter = 1
                while sheet_name in wb.sheetnames:
                    sheet_name = f"{base_name[:28]}_{counter}"
                    counter += 1
                
                add_chart_to_sheet(wb, sheet_name, data_sheet, time_col_idx, col_idx, col, 
                                 df, 'Time (sec)', col)
                chart_count += 1
            except Exception as e:
                print(f"  Warning: Could not create chart for {col}: {e}")
    
    # Save workbook
    wb.save(output_path)
    print(f"  ✓ Saved: {output_path}")
    print(f"  ✓ Created {chart_count} chart sheets\n")
    
    # Return dataframe for combined workbook creation
    return df

def ask_yes_no(question):
    """Ask a yes/no question via GUI"""
    answer = False
    
    def on_yes():
        nonlocal answer
        answer = True
        root.quit()
        root.destroy()
    
    def on_no():
        nonlocal answer
        answer = False
        root.quit()
        root.destroy()
    
    root = tk.Tk()
    root.title("Question")
    root.geometry("400x150")
    
    label = tk.Label(root, text=question, font=("Arial", 11))
    label.pack(pady=30)
    
    button_frame = tk.Frame(root)
    button_frame.pack(pady=10)
    
    yes_btn = tk.Button(button_frame, text="Yes", command=on_yes, 
                       bg="green", fg="white", font=("Arial", 10, "bold"), width=10)
    yes_btn.pack(side=tk.LEFT, padx=10)
    
    no_btn = tk.Button(button_frame, text="No", command=on_no, 
                      bg="red", fg="white", font=("Arial", 10, "bold"), width=10)
    no_btn.pack(side=tk.LEFT, padx=10)
    
    root.mainloop()
    
    return answer

def main():
    """
    Main function to process all CSV files in the Data directory
    """
    print("=" * 60)
    print("FSAE Data Post-Processing Script")
    print("=" * 60)
    print()
    
    # Get the script directory
    script_dir = Path(__file__).parent
    
    # Find all CSV files in the directory
    csv_files = list(script_dir.glob("*.csv"))
    
    if not csv_files:
        print("No CSV files found in the Data directory!")
        return
    
    print(f"Found {len(csv_files)} CSV file(s) to process")
    
    # Read first CSV to get column names for selection dialog
    first_csv = csv_files[0]
    print(f"\nReading column names from: {first_csv.name}")
    
    with open(first_csv, 'r', encoding='latin-1', errors='ignore') as f:
        lines = f.readlines()
    
    header_line = 0
    for i, line in enumerate(lines):
        if line.startswith('Time'):
            header_line = i
            break
    
    df_sample = pd.read_csv(first_csv, encoding='latin-1', skiprows=header_line, 
                            on_bad_lines='skip', nrows=1)
    df_sample.columns = df_sample.columns.str.strip()
    
    # Remove invalid columns
    valid_columns = [col for col in df_sample.columns 
                     if all(ord(c) < 128 for c in str(col))]
    
    # Show column selection dialog for individual analysis files
    print("\nStep 1: Select columns for individual analysis files")
    print("Opening column selection dialog...")
    selected_columns = select_columns_dialog(valid_columns, 
                                             "Select columns for individual analysis files:")
    
    if not selected_columns:
        print("\nNo columns selected. Exiting.")
        return
    
    print(f"\n✓ Selected {len(selected_columns)} columns for individual analysis")
    print("\nProcessing all CSV files...\n")
    
    # Process each CSV file with the same column selection
    all_dataframes = []
    for csv_file in csv_files:
        try:
            df = process_csv_file(str(csv_file), selected_columns)
            if df is not None:
                all_dataframes.append((csv_file.stem, df))
        except Exception as e:
            print(f"ERROR processing {csv_file}: {e}\n")
    
    # Ask if user wants combined workbook
    print("\nStep 2: Combined workbook")
    create_combined = ask_yes_no("Create a combined workbook with selected columns?")
    
    if create_combined and all_dataframes:
        # Show column selection for combined workbook
        print("\nOpening column selection for combined workbook...")
        combined_columns = select_columns_dialog(valid_columns,
                                                 "Select columns for combined workbook:")
        
        if not combined_columns:
            print("No columns selected for combined workbook.")
        else:
            # Get name for combined workbook
            combined_name = get_combined_workbook_name()
            
            print(f"\n{'=' * 60}")
            print(f"Creating combined workbook: {combined_name}.xlsx")
            print(f"{'=' * 60}")
            
            combined_wb = Workbook()
            combined_wb.remove(combined_wb.active)
            
            # For each file and selected column, create a separate sheet
            for file_name, df in all_dataframes:
                for col in combined_columns:
                    if col != 'Time (sec)' and col in df.columns:
                        print(f"  Adding sheet: {file_name} - {col}")
                        
                        # Create safe sheet name
                        safe_name = f"{file_name}_{col}".replace('/', '_').replace('(', '').replace(')', '').replace('#', '').replace(' ', '_')[:31]
                        sheet = combined_wb.create_sheet(safe_name)
                        
                        # Copy data to sheet
                        sheet.cell(1, 1, "Time (sec)")
                        sheet.cell(1, 2, col)
                        
                        for idx, (time_val, data_val) in enumerate(zip(df['Time (sec)'], df[col]), start=2):
                            sheet.cell(idx, 1, time_val)
                            sheet.cell(idx, 2, data_val)
                        
                        # Create chart
                        chart = LineChart()
                        chart.title = f"{file_name} - {col}"
                        chart.style = None
                        chart.x_axis.title = "Time (sec)"
                        chart.y_axis.title = col
                        chart.width = 20
                        chart.height = 12
                        
                        # Add gridlines for both axes
                        chart.x_axis.majorGridlines = ChartLines()
                        chart.y_axis.majorGridlines = ChartLines()
                        
                        chart.legend = None
                        
                        # Add data
                        y_values = Reference(sheet, min_col=2, min_row=1, max_row=len(df)+1)
                        x_values = Reference(sheet, min_col=1, min_row=2, max_row=len(df)+1)
                        chart.add_data(y_values, titles_from_data=True)
                        chart.set_categories(x_values)
                        
                        # Simple black line
                        if chart.series:
                            chart.series[0].graphicalProperties.line.solidFill = "000000"
                            chart.series[0].smooth = True
                        
                        # Position chart
                        sheet.add_chart(chart, "D2")
            
            # Save combined workbook
            combined_path = script_dir / f"{combined_name}.xlsx"
            combined_wb.save(combined_path)
            print(f"\n  ✓ Saved combined workbook: {combined_path}")
            print(f"  ✓ Created {len(all_dataframes) * len(combined_columns)} sheets\n")
    
    print("=" * 60)
    print("Processing complete!")
    print("=" * 60)

if __name__ == "__main__":
    main()
