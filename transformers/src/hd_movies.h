#pragma once
// HD movies: upscaled versions of the game's movies, shown over its picture while it plays them
// (hd_movies.cpp has the long form). TF_HD_MOVIES=<folder> ("movies_hd"), TF_FFMPEG=<ffmpeg.exe>.

#include <string>

class PS2Runtime;

namespace hd_movies
{
    // From the movie stream's creation and destruction (overrides.cpp, the game thread).
    void movieStarted(PS2Runtime &runtime, const std::string &name);
    void movieEnded();
    // Once a window frame, over the game's picture (main.cpp).
    void draw(PS2Runtime &runtime);
    bool playing();
    // The movie's name under FMV without its extension ("CG/EP01/N1_1"), and its upscaled file.
    std::string movieKey(std::string name);
    std::string fileFor(const std::string &key);
    // (TODO 341 (4)) The shape a movie is shown at, width / height of its picture (the band of rows the game shows,
    // overrides.cpp createFmvStream): the movies are MPEG-2 at 640x384 with square-pixel pictures - the films, the
    // attract film and the logos 640x360 between 12-row black bars (16:9), the Atari logo the whole 640x384 (5:3) - which
    // the PS2's 4:3 TV showed a little narrower (1.66:1 and 1.56:1). By default the shape they were made at;
    // TF_MOVIE_SHAPE=ps2: the PS2's. An HD copy is drawn at this shape too, whatever its file's own (the reforged films
    // are 1536x1082, 1.42:1 - shown at that, every cutscene came out about a fifth too narrow).
    float movieShape(const std::string &key);
    bool movieShapeAsPs2();
    // (TODO 341, the follow-up) The shape the playing movie's band is shown at now (overrides.cpp, each game frame while
    // it plays: its own, 16:9 or the window's - F1 Display "Menus and movies"): the HD copy is drawn over that rectangle.
    void setShownShape(float widthOverHeight);
}
