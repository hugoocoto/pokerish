#pragma once

#include "phevaluator/card.h"
#include "phevaluator/phevaluator.h"
#include "raylib.h"

#include <iostream>
#include <istream>
#include <string>

static Rectangle default_card_size{ 0, 0, 96, 128 };

// UI scale: user-facing level where 2.0 == the current look (96x128 cards,
// fonts 20, at a 1280x720 window). Auto-fit picks the largest of the levels
// {2.0, 1.5, 1.25, 1.0, 0.75, 0.5} that fits the window; --scale overrides.
// Everything in the GUIs goes through sc()/scf().
static float ui_scale = 2.0f;
static inline int sc(int v)
{
        return (int) (v * ui_scale / 2.0f + 0.5f);
}
static inline float scf(float v)
{
        return v * ui_scale / 2.0f;
}

class Card
{
    public:
        phevaluator::Card value;
        bool draw_colliders{ false };

        Card(std::string name);

        void draw(int x, int y, Color tint = WHITE);
        void change_theme(std::string filename);

        bool check_collision(Vector2) const; // with mouse or point colliders
        bool check_collision(Card) const;    // with other card
        int get_x() const;
        int get_y() const;
        int get_w() const;
        int get_h() const;

    private:
        void reload_texture();
        void update_positon(int x, int y);
        void update_width(int w, int h);
        Texture2D texture{};
        Image image{};
        std::string image_filename{};
        Rectangle collision_box{ default_card_size }; // default size
        Vector2 collision_point{
                default_card_size.x + default_card_size.width / 2,
                default_card_size.y + default_card_size.height / 2,
        };
};

Card::Card(std::string name)
{
        this->value          = phevaluator::Card(name);
        // this->draw_colliders = true; // debug
}

void
Card::update_positon(int x, int y)
{
        this->collision_box.x   = x;
        this->collision_box.y   = y;
        this->collision_point.x = x + this->collision_box.width / 2;
        this->collision_point.y = y + this->collision_box.height / 2;
}

void
Card::update_width(int w, int h)
{
        this->collision_box.width  = w;
        this->collision_box.height = h;
        this->update_positon(this->collision_box.x, this->collision_box.y);
}

void
Card::draw(int x, int y, Color tint)
{
        this->update_positon(x, y);

        if (this->draw_colliders) {
                int t = sc(2);
                DrawRectangleLinesEx({
                                     .x      = (float) (x - t),
                                     .y      = (float) (y - t),
                                     .width  = (float) (sc(96) + t * 2),
                                     .height = (float) (sc(128) + t * 2),
                                     },
                                     2 * t, BLUE);
        }

        this->reload_texture();

        if (IsTextureValid(this->texture)) {
                DrawTextureEx(this->texture, Vector2{ (float) x, (float) y }, 0.0f,
                              ui_scale / 2.0f, tint);
        } else {
                // so we can see if there is a card with an invalid texure
                DrawRectangle(x, y, sc(96), sc(128), RED);
                DrawRectangleLines(x, y, sc(96), sc(128), WHITE);
                DrawText("Invalid", x + sc(2), y + sc(2), sc(20), WHITE);
        }

        this->update_width(sc(96), sc(128)); // collision box matches the scaled draw

        if (this->draw_colliders) {
                DrawCircleV(this->collision_point, sc(2), BLUE);
        }
}

void
Card::reload_texture()
{
        // texture already valid
        if (IsTextureValid(this->texture)) return;

        // texture invalid but image valid
        if (IsImageValid(this->image)) {
                this->texture = LoadTextureFromImage(this->image);
                if (!IsTextureValid(this->texture)) {
                        printf("Texture loaded from image is not valid (1)\n");
                        exit(1);
                }

                return;
        }

        // both invalid: load image from file and then load the texture
        if (!FileExists(this->image_filename.c_str())) {
                printf("image %s does not exist\n", this->image_filename.c_str());
                exit(1);
        }

        this->image = LoadImage(this->image_filename.c_str());
        if (!IsImageValid(this->image)) {
                printf("image %s is not valid\n", this->image_filename.c_str());
                exit(1);
        }

        ImageResize(&this->image, this->get_w(), this->get_h());

        if (!IsImageValid(this->image)) {
                printf("image %s is not valid after resize\n", this->image_filename.c_str());
                exit(1);
        }

        this->texture = LoadTextureFromImage(this->image);
        if (!IsTextureValid(this->texture)) {
                printf("Texture loaded from image is not valid (2)\n");
                exit(1);
        }
}

void
Card::change_theme(std::string filename)
{
        this->image_filename = filename;
}

bool
Card::check_collision(Vector2 p) const
{
        return CheckCollisionPointRec(p, this->collision_box);
}

bool
Card::check_collision(Card c) const
{
        return CheckCollisionRecs(c.collision_box, this->collision_box);
}

int
Card::get_x() const
{
        return this->collision_box.x;
}

int
Card::get_y() const
{
        return this->collision_box.y;
}

int
Card::get_w() const
{
        return this->collision_box.width;
}

int
Card::get_h() const
{
        return this->collision_box.height;
}
