import gym
from rl_fusion.env import LvioFusionEnv

gym.envs.register(
     id='FactorGraph-v0',
     entry_point='rl_fusion:LvioFusionEnv',
     max_episode_steps=1000,
)