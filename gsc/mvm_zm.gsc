// mvm_zm.gsc - BO2 MVM zombie control for Plutonium T6 (Zombies)
// Driven by the BO2MVM overlay: it writes commands into the dvar "mvm_cmd" (separated by "|").
// Written against the real T6 zombie scripts (_zm_utility::spawn_zombie, set_zombie_run_cycle, zm_death deathanim).
#include maps\mp\zombies\_zm_utility;

init()
{
    level.mvm_ai = [];

    if ( !isdefined( level.mvm_ok ) )
        level.mvm_ok = [];

    level.mvm_spec = [];
    setdvar( "mvm_cmd", "" );
    setdvar( "mvm_track", "MVMT 0 0 0 0" );
    setdvar( "mvm_track2", "MVMT 0 0 0 0" );
    level.mvm_track_id = -1;
    level.mvm_track2_id = -1;
    level.mvm_failsafe_orig = 1;

    if ( isdefined( level.zombie_vars ) && isdefined( level.zombie_vars["zombie_use_failsafe"] ) )
        level.mvm_failsafe_orig = level.zombie_vars["zombie_use_failsafe"];

    setdvar( "mvm_dof", "" );
    setdvar( "mvm_film", "" );
    level thread mvm_track_loop();
    level thread mvm_loop();
    level thread mvm_fx_loop();
    level thread mvm_ready();
}

// live post effects the overlay asks for. They are set from the script because the console route is cheat locked on some builds.
// mvm_dof  = "nearStart nearEnd farStart farEnd nearBlur farBlur" or "off"
// mvm_film = "on brightness contrast desaturation" or "off"
mvm_fx_loop()
{
    level endon( "end_game" );
    last_dof = "";
    last_film = "";
    t = 0;

    for ( ;; )
    {
        wait 0.1;
        t = t + 0.1;
        dof = getdvar( "mvm_dof" );

        if ( dof != "" && ( dof != last_dof || t > 1.5 ) )
        {
            last_dof = dof;
            mvm_apply_dof( dof );
        }
        else if ( dof == "" && last_dof != "" )
        {
            last_dof = "";
            mvm_apply_dof( "off" );
        }

        film = getdvar( "mvm_film" );

        if ( film != "" && ( film != last_film || t > 1.5 ) )
        {
            last_film = film;
            mvm_apply_film( film );
        }
        else if ( film == "" && last_film != "" )
        {
            last_film = "";
            mvm_apply_film( "off" );
        }

        if ( t > 1.5 )
            t = 0;
    }
}

mvm_apply_dof( s )
{
    players = level.players;

    for ( i = 0; i < players.size; i++ )
    {
        if ( s == "off" )
        {
            players[i] setdepthoffield( 0, 0, 7000, 10000, 6, 1.8 );
            continue;
        }

        a = strtok( s, " " );

        if ( a.size < 6 )
            continue;

        players[i] setdepthoffield( float( a[0] ), float( a[1] ), float( a[2] ), float( a[3] ), float( a[4] ), float( a[5] ) );
    }
}

mvm_apply_film( s )
{
    if ( s == "off" )
    {
        setdvar( "r_filmUseTweaks", 0 );
        setdvar( "r_filmTweakEnable", 0 );
        return;
    }

    a = strtok( s, " " );

    if ( a.size < 4 )
        return;

    setdvar( "r_filmUseTweaks", 1 );
    setdvar( "r_filmTweakEnable", 1 );
    setdvar( "r_filmTweakBrightness", float( a[1] ) );
    setdvar( "r_filmTweakContrast", float( a[2] ) );
    setdvar( "r_filmTweakDesaturation", float( a[3] ) );
}

// lock-on / depth-of-field focus: reports where the tracked zombie (id), the player (0) or the middle of all zombies (-2) is.
// slot 0 = lock-on (dvar mvm_track), slot 1 = focus (dvar mvm_track2). Format: "MVMT x*10 y*10 z*10 extra"
mvm_track_loop()
{
    level endon( "end_game" );

    for ( ;; )
    {
        wait 0.05;
        mvm_track_report( level.mvm_track_id, "mvm_track" );
        mvm_track_report( level.mvm_track2_id, "mvm_track2" );
    }
}

mvm_track_report( id, name )
{
    if ( !isdefined( id ) || id == -1 )
        return;

    if ( id == -2 )
    {
        keys = getarraykeys( level.mvm_ai );
        pts = [];
        sum = ( 0, 0, 0 );

        for ( i = 0; i < keys.size; i++ )
        {
            ai = level.mvm_ai[keys[i]];

            if ( !isdefined( ai ) || !isalive( ai ) )
                continue;

            p = ai.origin + ( 0, 0, 50 );
            pts[pts.size] = p;
            sum = sum + p;
        }

        if ( pts.size == 0 )
            return;

        c = ( sum[0] / pts.size, sum[1] / pts.size, sum[2] / pts.size );
        r = 0;

        for ( j = 0; j < pts.size; j++ )
        {
            d = distance( c, pts[j] );

            if ( d > r )
                r = d;
        }

        setdvar( name, "MVMT " + int( c[0] * 10 ) + " " + int( c[1] * 10 ) + " " + int( c[2] * 10 ) + " " + int( r * 10 ) );
        return;
    }

    ent = undefined;

    if ( id == 0 )
    {
        pl = getplayers();

        if ( pl.size > 0 )
            ent = pl[0];
    }
    else
        ent = level.mvm_ai[id];

    if ( !isdefined( ent ) )
        return;

    p = ent.origin + ( 0, 0, 50 );

    if ( id != 0 && isalive( ent ) )
    {
        t = ent gettagorigin( "j_spine4" );

        if ( isdefined( t ) )
            p = t;
    }

    setdvar( name, "MVMT " + int( p[0] * 10 ) + " " + int( p[1] * 10 ) + " " + int( p[2] * 10 ) + " 0" );
}

mvm_ready()
{
    level endon( "end_game" );
    wait 6;
    iprintln( "^2BO2 MVM zombie control ready" );
}

mvm_loop()
{
    level endon( "end_game" );

    for ( ;; )
    {
        cmd = getdvar( "mvm_cmd" );

        if ( cmd != "" )
        {
            setdvar( "mvm_cmd", "" );
            list = strtok( cmd, "|" );

            for ( i = 0; i < list.size; i++ )
                mvm_exec( list[i] );
        }

        wait 0.05;
    }
}

mvm_exec( line )
{
    a = strtok( line, " " );

    if ( a.size < 1 )
        return;

    c = a[0];

    if ( c == "track" )
        level.mvm_track_id = int( a[1] );
    else if ( c == "track2" )
        level.mvm_track2_id = int( a[1] );
    else if ( c == "spawn" )
        mvm_spawn( int( a[1] ), float( a[2] ), float( a[3] ), a[4], a[5], mvm_view( a ) );
    else if ( c == "tp" )
        mvm_tp( int( a[1] ), mvm_view( a ), 0 );
    else if ( c == "tpall" )
        mvm_tpall( mvm_view( a ) );
    else if ( c == "look" )
        mvm_look( int( a[1] ), mvm_view( a ) );
    else if ( c == "lookall" )
        mvm_lookall( mvm_view( a ) );
    else if ( c == "act" )
        mvm_act( int( a[1] ), a[2] );
    else if ( c == "all" )
        mvm_all( a[1] );
    else if ( c == "pmodel" )
        mvm_pmodel( a[1] );
    else if ( c == "pdefault" )
        mvm_pdefault();
    else if ( c == "clear" )
        mvm_clear();
    else if ( c == "reset" )
        mvm_reset();
    else if ( c == "hud" )
        mvm_hud( int( a[1] ) );
    else if ( c == "head" )
        mvm_head( int( a[1] ) );
    else if ( c == "god" )
        mvm_god( int( a[1] ) );
}

mvm_hud( on )
{
    players = level.players;

    for ( i = 0; i < players.size; i++ )
        players[i] setclientuivisibilityflag( "hud_visible", on );
}

mvm_god( on )
{
    players = level.players;

    for ( i = 0; i < players.size; i++ )
    {
        if ( on )
            players[i] enableinvulnerability();
        else
            players[i] disableinvulnerability();
    }
}

// first person view out of a zombie's head (id 0 = back to normal)
mvm_head( id )
{
    p = level.players[0];

    if ( !isdefined( p ) )
        return;

    p unlink();

    if ( isdefined( level.mvm_head_ent ) )
    {
        level.mvm_head_ent delete();
        level.mvm_head_ent = undefined;
    }

    if ( id <= 0 )
        return;

    ai = level.mvm_ai[id];

    if ( !isdefined( ai ) )
        return;

    // an anchor that follows the zombie's head every frame, the player is linked to it
    anchor = spawn( "script_model", ai gettagorigin( "j_head" ) );
    anchor setmodel( "tag_origin" );
    level.mvm_head_ent = anchor;
    anchor thread mvm_anchor_follow( ai );
    p playerlinktodelta( anchor, "tag_origin", 1, 180, 180, 80, 80, 1 );
}

mvm_anchor_follow( ai )
{
    self endon( "death" );

    while ( isdefined( ai ) )
    {
        self.origin = ai gettagorigin( "j_head" ) + anglestoforward( ai.angles ) * 6;
        self.angles = ai.angles;
        wait 0.05;
    }
}

// camera pose sent by the overlay as "v x y z pitch yaw" (free camera); without it the player's own view is used
mvm_view( a )
{
    p = level.players[0];
    v = [];
    v["org"] = p geteye();
    v["ang"] = p getplayerangles();
    v["body"] = p.origin;

    for ( i = 0; i < a.size; i++ )
    {
        if ( a[i] == "v" && isdefined( a[i + 5] ) )
        {
            v["org"] = ( float( a[i + 1] ), float( a[i + 2] ), float( a[i + 3] ) );
            v["ang"] = ( float( a[i + 4] ), float( a[i + 5] ), 0 );
            v["body"] = v["org"];
            break;
        }
    }

    return v;
}

// where the crosshair points (first thing the view ray hits), with a sideways offset
mvm_cross( v, side )
{
    fwd = anglestoforward( v["ang"] );
    t = bullettrace( v["org"], v["org"] + fwd * 12000, 0, level.players[0] );

    if ( t["fraction"] >= 1 )
        pos = v["org"] + fwd * 350;          // looking at sky / nothing: drop it in front of the camera
    else
    {
        pos = t["position"];

        if ( t["fraction"] * 12000 < 600 )
            pos = pos - fwd * 25;            // keep off the wall it hit
    }

    right = anglestoright( ( 0, v["ang"][1], 0 ) );
    pos = pos + right * side;
    return mvm_ground( pos, v["body"] );
}

// snap a point onto the floor; if nothing solid is found there, fall back to the player's own height so it never spawns in the void
mvm_ground( pos, ref )
{
    t2 = bullettrace( pos + ( 0, 0, 80 ), pos - ( 0, 0, 300 ), 0, undefined );

    if ( t2["fraction"] < 1 )
        return t2["position"];

    t3 = bullettrace( pos + ( 0, 0, 400 ), pos - ( 0, 0, 40 ), 0, undefined );

    if ( t3["fraction"] < 1 )
        return t3["position"];

    return ( pos[0], pos[1], ref[2] - 40 );
}

mvm_yaw_to( from, to )
{
    d = to - from;
    a = vectortoangles( ( d[0], d[1], 0 ) );
    return a[1];
}

// move a zombie: locked ones move their anchor, free ones are teleported
mvm_place( ai, pos, yaw )
{
    if ( isdefined( ai.mvm_id ) && isdefined( level.mvm_spec[ai.mvm_id] ) )
    {
        level.mvm_spec[ai.mvm_id].pos = pos;      // a moved zombie starts the next take where you put it
        level.mvm_spec[ai.mvm_id].yaw = yaw;
    }

    if ( isdefined( ai.mvm_anchor ) )
    {
        ai unlink();
        ai.mvm_anchor.origin = pos;
        ai.mvm_anchor.angles = ( 0, yaw, 0 );
        ai forceteleport( pos, ( 0, yaw, 0 ) );
        ai linkto( ai.mvm_anchor );
    }
    else
        ai forceteleport( pos, ( 0, yaw, 0 ) );
}

mvm_tp( id, v, side )
{
    ai = level.mvm_ai[id];

    if ( !isdefined( ai ) || !isalive( ai ) )
        return;

    pos = mvm_cross( v, side );
    mvm_place( ai, pos, mvm_yaw_to( pos, v["body"] ) );
}

mvm_tpall( v )
{
    keys = getarraykeys( level.mvm_ai );
    n = 0;

    for ( i = 0; i < keys.size; i++ )
    {
        if ( isdefined( level.mvm_ai[keys[i]] ) )
        {
            mvm_tp( keys[i], v, ( n - ( keys.size - 1 ) * 0.5 ) * 80 );
            n++;
        }
    }
}

mvm_look( id, v )
{
    ai = level.mvm_ai[id];

    if ( !isdefined( ai ) || !isalive( ai ) )
        return;

    mvm_place( ai, ai.origin, mvm_yaw_to( ai.origin, v["body"] ) );
}

mvm_lookall( v )
{
    keys = getarraykeys( level.mvm_ai );

    for ( i = 0; i < keys.size; i++ )
        mvm_look( keys[i], v );
}

mvm_spawn( id, dist, side, mode, model, v )
{
    p = level.players[0];

    if ( !isdefined( p ) )
        return;

    if ( !isdefined( level.zombie_spawners ) || level.zombie_spawners.size < 1 )
    {
        iprintln( "^1MVM: no zombie spawners on this map" );
        return;
    }

    if ( dist < 0 )
        pos = mvm_cross( v, side );   // at the crosshair
    else
    {
        ang = ( 0, v["ang"][1], 0 );
        wall = bullettrace( v["body"], v["body"] + anglestoforward( ang ) * dist, 0, level.players[0] );

        if ( wall["fraction"] < 1 )
        {
            dist = dist * wall["fraction"] - 40;

            if ( dist < 50 )
                dist = 50;
        }

        pos = v["body"] + anglestoforward( ang ) * dist + anglestoright( ang ) * side;
        pos = mvm_ground( pos, v["body"] );
    }

    yaw = mvm_yaw_to( pos, v["body"] );

    // remember exactly how this zombie was placed, so "reset" can put it back for the next take
    spec = spawnstruct();
    spec.pos = pos;
    spec.yaw = yaw;
    spec.mode = mode;
    spec.model = model;
    level.mvm_spec[id] = spec;

    mvm_spawn_at( id, pos, yaw, mode, model );
}

mvm_spawn_at( id, pos, yaw, mode, model )
{
    sp = level.zombie_spawners[0];
    sp.script_forcespawn = 1;
    sp.origin = pos;
    sp.angles = ( 0, yaw, 0 );

    // the game's round failsafe kills any zombie that stands still for 30 s: switch it off just while this one spawns
    level.zombie_vars["zombie_use_failsafe"] = 0;
    ai = spawn_zombie( sp, "mvm_zombie", sp );
    level thread mvm_failsafe_restore();

    if ( !isdefined( ai ) )
    {
        iprintln( "^1MVM: spawn failed" );
        return;
    }

    ai.mvm_id = id;
    ai.ignore_cleanup_mgr = 1;      // stop the round cleanup from deleting it
    ai.no_powerups = 1;
    ai.ignore_enemy_count = 1;
    ai.ignore_round_spawn_failsafe = 1;
    level.mvm_ai[id] = ai;
    ai thread mvm_setup( pos, yaw, mode, model );
    ai thread mvm_guard( pos, yaw );
}

mvm_failsafe_restore()
{
    wait 1.5;

    if ( isdefined( level.mvm_failsafe_orig ) )
        level.zombie_vars["zombie_use_failsafe"] = level.mvm_failsafe_orig;
}

// next take: remove every zombie we made (alive, dead or leftover bodies) and respawn each one exactly where it started
mvm_reset()
{
    mvm_wipe();
    keys = getarraykeys( level.mvm_spec );

    for ( i = 0; i < keys.size; i++ )
    {
        sp = level.mvm_spec[keys[i]];
        mvm_spawn_at( keys[i], sp.pos, sp.yaw, sp.mode, sp.model );
    }
}

// keeps a zombie alive and in the map: refills health, undoes out-of-world drifts, stops the game killing it
mvm_guard( pos, yaw )
{
    self endon( "death" );
    self endon( "mvm_gone" );
    wait 1.5;

    for ( ;; )
    {
        self.ignore_cleanup_mgr = 1;

        if ( getdvarint( "mvm_god" ) == 1 )
        {
            self.maxhealth = 1000000;
            self.health = 1000000;
        }

        if ( isdefined( self.mvm_id ) && isdefined( level.mvm_spec[self.mvm_id] ) )
        {
            p = level.mvm_spec[self.mvm_id].pos;

            if ( self.origin[2] < p[2] - 150 || distance( self.origin, p ) > 6000 )
                mvm_place( self, p, level.mvm_spec[self.mvm_id].yaw );
        }

        wait 0.25;
    }
}

mvm_setup( pos, yaw, mode, model )
{
    self endon( "death" );
    wait 0.8;
    self notify( "goal" );          // ends zombie_assure_node, which kills a zombie that never reaches an entrance
    self forceteleport( pos, ( 0, yaw, 0 ) );

    if ( isdefined( model ) && model != "default" )
    {
        parts = strtok( model, "," );   // body[,head[,hat/hair...]]

        if ( mvm_models_ok( parts, self ) )
        {
            self detachall();
            self setmodel( parts[0] );

            for ( i = 1; i < parts.size; i++ )
            {
                self attach( parts[i], "", 1 );

                if ( i == 1 )
                    self.headmodel = parts[i];
            }
        }
    }

    self.mvm = 1;
    self mvm_mode( mode );
}

// idle = frozen in place (locked to an anchor, AI chase/entrance logic cancelled), otherwise a real zombie move speed
mvm_mode( m )
{
    self endon( "death" );
    self notify( "mvm_mode_change" );

    if ( m == "idle" )
    {
        self thread mvm_hold();
        return;
    }

    self mvm_unlock();

    // hand control back to the normal zombie AI at the requested speed
    self notify( "stop_find_flesh" );
    self notify( "stop_zombie_goto_entrance" );
    wait 0.1;
    self.ignoreall = 0;
    self.ignoreme = 0;
    self.has_legs = 1;

    if ( m == "stumpy" )
    {
        self.has_legs = 0;
        self set_zombie_run_cycle( "walk" );
        self.zombie_move_speed = "stumpy";
        self maps\mp\animscripts\zm_run::needsupdate();
    }
    else
        self set_zombie_run_cycle( m );

    self thread maps\mp\zombies\_zm_ai_basic::find_flesh();
}

mvm_hold()
{
    self endon( "death" );
    self endon( "mvm_mode_change" );
    self mvm_lock();

    for ( ;; )
    {
        self.ignoreall = 1;
        self.ignoreme = 1;
        self notify( "stop_find_flesh" );
        self notify( "stop_zombie_goto_entrance" );
        self setgoalpos( self.origin );
        wait 0.2;
    }
}

// pin the zombie to an invisible anchor so nothing can move it
mvm_lock()
{
    if ( isdefined( self.mvm_anchor ) )
        return;

    a = spawn( "script_origin", self.origin );
    a.angles = self.angles;
    self.mvm_anchor = a;
    self linkto( a );
    self thread mvm_anchor_cleanup( a );
}

mvm_anchor_cleanup( a )
{
    self waittill( "death" );

    if ( isdefined( a ) )
        a delete();
}

mvm_unlock()
{
    if ( !isdefined( self.mvm_anchor ) )
        return;

    self unlink();
    self.mvm_anchor delete();
    self.mvm_anchor = undefined;
}

mvm_act( id, act )
{
    ai = level.mvm_ai[id];

    if ( !isdefined( ai ) )
        return;

    mvm_do( ai, act );
}

mvm_all( act )
{
    keys = getarraykeys( level.mvm_ai );

    for ( i = 0; i < keys.size; i++ )
    {
        ai = level.mvm_ai[keys[i]];

        if ( isdefined( ai ) )
            mvm_do( ai, act );
    }
}

mvm_clear()
{
    mvm_wipe();
    level.mvm_spec = [];
}

mvm_wipe()
{
    keys = getarraykeys( level.mvm_ai );

    for ( i = 0; i < keys.size; i++ )
    {
        ai = level.mvm_ai[keys[i]];

        if ( isdefined( ai ) )
        {
            ai mvm_unlock();
            ai delete();
        }
    }

    level.mvm_ai = [];
}

mvm_do( ai, act )
{
    if ( act == "idle" || act == "walk" || act == "run" || act == "sprint" || act == "super_sprint" || act == "stumpy" )
    {
        if ( isalive( ai ) )
            ai mvm_mode( act );

        return;
    }

    if ( act == "remove" )
    {
        ai mvm_unlock();
        ai delete();
        return;
    }

    if ( !isalive( ai ) )
        return;

    ai notify( "mvm_mode_change" );
    ai mvm_unlock();

    p = level.players[0];
    loc = "none";
    mod = "MOD_RIFLE_BULLET";
    anim_state = undefined;

    // death animation states that exist in the zombie ASD (see animscripts\zm_death.gsc: self.deathanim)
    if ( act == "d_normal" )         anim_state = "zm_death";
    else if ( act == "d_jetgun" )    anim_state = "zm_jetgun_death";
    else if ( act == "d_jetgun_s" )  anim_state = "zm_jetgun_sprint_death";
    else if ( act == "d_jetgun_c" )  anim_state = "zm_jetgun_death_crawl";
    else if ( act == "k_head" )      loc = "head";
    else if ( act == "k_torso" )     loc = "torso_upper";
    else if ( act == "k_legs" )      loc = "left_leg_lower";
    else if ( act == "k_arms" )      loc = "left_arm_upper";
    else if ( act == "k_melee" )     mod = "MOD_MELEE";
    else if ( act == "k_explode" )   mod = "MOD_GRENADE_SPLASH";
    else if ( act == "k_burn" )      mod = "MOD_BURNED";

    if ( isdefined( anim_state ) )
    {
        ai.deathanim = anim_state;
        ai.deathanim_substate = undefined;
    }

    ai.allowdeath = 1;
    ai dodamage( ai.health + 10000, ai.origin, p, p, loc, mod );
}


// a model can only be used if the game has it precached: either the map did, or the generated mvm_models.gsc did
mvm_models_ok( parts, ent )
{
    for ( i = 0; i < parts.size; i++ )
    {
        n = parts[i];

        if ( isdefined( level.mvm_ok[n] ) )
            continue;

        if ( isdefined( ent.model ) && ent.model == n )
            continue;

        found = false;
        cnt = ent getattachsize();

        for ( k = 0; k < cnt; k++ )
        {
            if ( ent getattachmodelname( k ) == n )
                found = true;
        }

        if ( found )
            continue;

        iprintln( "^3MVM: " + n + " is not unlocked yet - start a new match once (see the zombies tab)" );
        return false;
    }

    return true;
}

// change what the player's own character looks like (visible in free camera / third person)
mvm_pmodel( model )
{
    p = level.players[0];

    if ( !isdefined( p ) )
        return;

    if ( model == "default" )
    {
        mvm_pdefault();
        return;
    }

    parts = strtok( model, "," );

    if ( !mvm_models_ok( parts, p ) )
        return;

    p detachall();
    p setmodel( parts[0] );

    for ( i = 1; i < parts.size; i++ )
        p attach( parts[i], "", 1 );
}

mvm_pdefault()
{
    p = level.players[0];

    if ( !isdefined( p ) )
        return;

    p detachall();

    if ( isdefined( level.givecustomcharacters ) )
        p [[ level.givecustomcharacters ]]();
}
