import tkinter as tk
from tkinter import simpledialog

# Standard International Morse Code Dictionary
MORSE_CODE_DICT = {
    'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.',
    'F': '..-.', 'G': '--.', 'H': '....', 'I': '..', 'J': '.---',
    'K': '-.-', 'L': '.-..', 'M': '--', 'N': '-.', 'O': '---',
    'P': '.--.', 'Q': '--.-', 'R': '.-.', 'S': '...', 'T': '-',
    'U': '..-', 'V': '...-', 'W': '.--', 'X': '-..-', 'Y': '-.--',
    'Z': '--..', '1': '.----', '2': '..---', '3': '...--',
    '4': '....-', '5': '.....', '6': '-....', '7': '--...',
    '8': '---..', '9': '----.', '0': '-----', ', ': '--..--',
    '.': '.-.-.-', '?': '..--..', '/': '-..-.', '-': '-....-',
    '(': '-.--.', ')': '-.--.-'
}

class DistantLightApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Distant Morse Light")
        self.root.geometry("800x600")
        self.root.configure(bg='black')
        
        # Press Escape to close the app easily
        self.root.bind("<Escape>", lambda e: self.root.quit())

        # Create a completely black canvas with no borders
        self.canvas = tk.Canvas(root, bg='black', highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)

        # Draw the distant light (initialized as black/invisible)
        # Radius is kept very small (2 pixels) to look far away
        self.light_radius = 10
        self.light = self.canvas.create_oval(
            0, 0, 0, 0, fill='black', outline='black'
        )

        # Keep the dot perfectly centered even if the window is resized
        self.canvas.bind("<Configure>", self.center_light)

        # Timing variables (in milliseconds)
        self.base_unit = 100
        self.sequence = []

        # Ask for user input shortly after the black window appears
        self.root.after(500, self.get_input)

    def center_light(self, event):
        """Calculates the center of the window and moves the dot there."""
        x = event.width / 2
        y = event.height / 2
        r = self.light_radius
        self.canvas.coords(self.light, x - r, y - r, x + r, y + r)

    def get_input(self):
        """Prompts the user for a message."""
        message = simpledialog.askstring(
            "Transmission Input", 
            "Enter message to transmit (Press ESC later to exit):", 
            parent=self.root
        )
        
        if message:
            self.build_sequence(message.upper())
            # Wait 1.5 seconds before starting so the user's eyes adjust to the dark
            self.root.after(1500, self.process_sequence)
        else:
            # Quit if the user cancels the prompt
            self.root.quit()

    def build_sequence(self, text):
        """Converts text into a queued sequence of ON/OFF states and durations."""
        
        # --- CUSTOM TIMING RULES ---
        dot_len = self.base_unit
        dash_len = self.base_unit * 2   # Custom: Dash is 2x longer than a dot
        
        # Standard spacing rules (scaled to base unit)
        intra_char_gap = self.base_unit     # Gap between dots/dashes in the same letter
        inter_char_gap = self.base_unit * 3 # Gap between letters
        word_gap = self.base_unit * 7       # Gap between words

        words = text.split(' ')
        for i, word in enumerate(words):
            for j, char in enumerate(word):
                if char in MORSE_CODE_DICT:
                    code = MORSE_CODE_DICT[char]
                    
                    # Add flashes for the character
                    for k, symbol in enumerate(code):
                        if symbol == '.':
                            self.sequence.append(('ON', dot_len))
                        elif symbol == '-':
                            self.sequence.append(('ON', dash_len))

                        # Add gap between symbols (except after the last symbol of the letter)
                        if k < len(code) - 1:
                            self.sequence.append(('OFF', intra_char_gap))

                    # Add gap between letters (except after the last letter of the word)
                    if j < len(word) - 1:
                        self.sequence.append(('OFF', inter_char_gap))

            # Add gap between words (except after the very last word)
            if i < len(words) - 1:
                self.sequence.append(('OFF', word_gap))

        # Ensure the light turns off completely at the end of the message
        self.sequence.append(('OFF', 0))

    def process_sequence(self):
        """Recursively processes the queue to animate the flashes."""
        if not self.sequence:
            # When finished, prompt for a new message
            self.root.after(1000, self.get_input)
            return

        state, duration = self.sequence.pop(0)

        if state == 'ON':
            # Turn light on (pure white)
            self.canvas.itemconfig(self.light, fill='white', outline='white')
        else:
            # Turn light off (pitch black)
            self.canvas.itemconfig(self.light, fill='black', outline='black')

        if duration > 0:
            # Schedule the next change in the sequence
            self.root.after(duration, self.process_sequence)
        else:
            self.process_sequence()

if __name__ == "__main__":
    root = tk.Tk()
    app = DistantLightApp(root)
    # Start the application
    root.mainloop()