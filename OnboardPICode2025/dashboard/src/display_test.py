import pygame

# Initialize display
pygame.init()
screen = pygame.display.set_mode((800, 480))  # Match display resolution
pygame.display.set_caption("FSAE Dashboard")

# Colors
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)

# Main loop
running = True
while running:
    for event in pygame.event.get():
        if event.type == pygame.QUIT:
            running = False
    
    screen.fill(BLACK)
    font = pygame.font.Font(None, 50)
    text = font.render("RPM: Hello World", True, WHITE)
    screen.blit(text, (50, 50))
    
    pygame.display.flip()

pygame.quit()
