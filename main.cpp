#include "isoftplayer.h"
#include "utils.h"

int main(int argc, char ** argv)
{
    if (argc < 2) {
        ms_debug("need filename\n");
        return 1;
    }

    QApplication app(argc, argv);

    av_register_all();
    av_log_set_level(AV_LOG_DEBUG);

    MediaState *ms = mediastate_init(argv[1]);
    ms->player->show();

    app.exec();

    mediastate_close(ms);
    return 0;
}
