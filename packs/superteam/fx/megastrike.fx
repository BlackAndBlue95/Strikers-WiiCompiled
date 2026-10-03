# Super Team's Mega Strike effects: Super Mario Strikers' Super Strike effects for the robot team (its
# "mystery" ones, from your SMS disc), arranged as Charged's Mega Strike plays them. smsfx2bun.py --extra
# builds these into art/effects/superteam_sms.bun; code/superteam.cpp says when its cutscenes play them.
# Templates and groups by SMS's names (lower case), as SMS's scripts.fx / templates.fx have them.

# Its charge-up as it winds up: SMS's Super Strike windup, every captain's.
group superteam_sts_windup from ball_sts_windup
end

# The vortex around it as it floats before the kick: lightning, rings and glow.
group superteam_sts_vortex from mystery_captain_sts_effect
end

# The kick.
group superteam_sts_shot from mystery_shoot_to_score_shot
end

# Its balls in flight in the last cutscene: SMS's yellow fireball, lightning and embers.
template superteam_lightning_burst from fx_dk_sts_test
    number 300 0
end
group superteam_megastrike_ball
    play fx_fireball_thick_nis_yellow at ball layer 3
    play superteam_lightning_burst at ball
    play fx_shot_fire_ball_embers at ball
end

# The same on the balls in play after the cutscenes, for as long as a ball flies (SMS's ball reached the
# goal within a second; Charged's fly longer). The game names this one after the character.
template superteam_fireball_trail from fx_fireball_thick_nis_yellow
    fountainlife 100
end
template superteam_lightning_trail from fx_dk_sts_test
    fountainlife 100
    number 120 0
end
template superteam_embers_trail from fx_shot_fire_ball_embers
    fountainlife 100
end
group superteam_megastrike_home_3_gameplay
    play superteam_fireball_trail at ball layer 3
    play superteam_lightning_trail at ball
    play superteam_embers_trail at ball
end
