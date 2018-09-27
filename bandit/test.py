import spynnaker8 as p
from spynnaker.pyNN.connections.\
    spynnaker_live_spikes_connection import SpynnakerLiveSpikesConnection
from spynnaker.pyNN.spynnaker_external_device_plugin_manager import \
    SpynnakerExternalDevicePluginManager as ex
import spinn_bandit


p.setup()

probabilities = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6]

input_pop = p.Population(len(probabilities), p.SpikeSourcePoisson(rate=5))

arms_pop = p.Population(1, spinn_bandit.Bandit(probabilities, 200))

p.Projection(input_pop, arms_pop, p.AllToAllConnector)

p.run(10000)





