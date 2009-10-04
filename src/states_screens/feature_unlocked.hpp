#ifndef HEADER_FEATURE_UNLOCKED_HPP
#define HEADER_FEATURE_UNLOCKED_HPP

#include "guiengine/cutscene.hpp"

namespace irr { namespace scene { class ISceneNode; class ICameraSceneNode; class ILightSceneNode; } }

class FeatureUnlockedCutScene : public GUIEngine::CutScene
{
    FeatureUnlockedCutScene();
    void prepare();
    
    /** sky angle, 0-360 */
    float m_angle;
    
    /** Key position from origin (where the chest is) */
    float m_key_pos;
    
    /** Angle of the key (from 0 to 1, simply traces progression) */
    float m_key_angle;
    
    irr::scene::ISceneNode* m_sky;
    irr::scene::ICameraSceneNode* m_camera;
    irr::scene::ISceneNode* m_chest;
    irr::scene::ISceneNode* m_chest_top;
    irr::scene::ISceneNode* m_key;
    irr::scene::ILightSceneNode* m_light;
public:
    static void show();
  
    void onUpdate(float dt, irr::video::IVideoDriver*);
    void terminate();
};

#endif

